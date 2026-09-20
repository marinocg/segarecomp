#!/usr/bin/env python3

import importlib.util
import json
import os
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ghidra_tool", PROJECT_ROOT / "tools" / "ghidra.py")
assert SPEC is not None and SPEC.loader is not None
GHIDRA_TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GHIDRA_TOOL)

COMPOSE_TEXT = (PROJECT_ROOT / "tools" / "ghidra" / "compose.yaml").read_text(encoding="utf-8")


def _service_block(text: str, name: str) -> str:
    lines = text.splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == f"{name}:")
    indent = len(lines[start]) - len(lines[start].lstrip())
    end = len(lines)
    for i in range(start + 1, len(lines)):
        stripped = lines[i].strip()
        if stripped and not stripped.startswith("#"):
            current = len(lines[i]) - len(lines[i].lstrip())
            if current <= indent:
                end = i
                break
    return "\n".join(lines[start:end])


def _strip_comments(text: str) -> str:
    out = []
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        out.append(line.split(" #", 1)[0] if " #" in line else line)
    return "\n".join(out)


class GhidraToolTest(unittest.TestCase):
    def test_runtime_creation_is_idempotent_and_private(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "runtime"
            first = GHIDRA_TOOL.ensure_runtime(root)
            first_token = GHIDRA_TOOL.parse_env(first)["GHIDRA_MCP_AUTH_TOKEN"]
            second = GHIDRA_TOOL.ensure_runtime(root)

            self.assertEqual(first, second)
            self.assertEqual(first_token, GHIDRA_TOOL.parse_env(second)["GHIDRA_MCP_AUTH_TOKEN"])
            self.assertEqual(len(first_token), 64)
            if os.name != "nt":  # POSIX permission bits only
                self.assertEqual(stat.S_IMODE(first.stat().st_mode), 0o600)
            self.assertTrue((root / "input").is_dir())
            self.assertEqual(
                GHIDRA_TOOL.parse_env(first)["GHIDRA_INPUT_DIR"], str((root / "input").resolve())
            )

    def test_compose_command_uses_pinned_project_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "runtime"
            command = GHIDRA_TOOL.compose_command(root, "config", "--quiet")
            self.assertEqual(command[:2], ["docker", "compose"])
            self.assertIn(str(PROJECT_ROOT / "tools" / "ghidra" / "compose.yaml"), command)
            self.assertIn(str(root / "stack.env"), command)

    def test_running_services_accepts_newline_delimited_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "runtime"
            result = GHIDRA_TOOL.subprocess.CompletedProcess(
                args=[],
                returncode=0,
                stdout='{"Service":"ghidra"}\n{"Service":"bridge"}\n',
            )
            with patch.object(GHIDRA_TOOL.subprocess, "run", return_value=result):
                self.assertEqual(GHIDRA_TOOL.running_services(root), {"bridge", "ghidra"})


class GhidraMcpReproductionTest(unittest.TestCase):
    """SEG-007-T162 Scope §1: deterministic offline reproduction of the broken
    agent-accessible Ghidra MCP path.

    A fully offline assertion of the *live* failure is impossible without the
    running bridge container (the failure is produced by the pinned bridge's
    runtime loopback-URL validation and its empty ``list_instances``). This test
    therefore captures the identical contract at the Compose-config level, which
    is what actually drives that runtime behavior:

    * Pre-repair, the ``bridge`` service had its own ``networks: [backend]`` entry
      and ``GHIDRA_MCP_URL: http://ghidra:8089/``. The bridge shared no network
      namespace with ``ghidra``, so ``list_instances`` was empty, and the
      compose-service URL is non-loopback, so ``connect_instance`` was rejected
      with ``Expected http://<127.0.0.1|localhost|::1>:<port>``.
    * Post-repair, the ``bridge`` service uses ``network_mode: service:ghidra``
      (mutually exclusive with a ``networks:`` list) and a loopback
      ``GHIDRA_MCP_URL`` / ``GHIDRA_SERVER_URL`` of ``http://127.0.0.1:8089``,
      which the pinned bridge accepts and auto-connects.

    This test fails against the pre-repair compose.yaml and passes after it.
    """

    def setUp(self) -> None:
        self.bridge = _strip_comments(_service_block(COMPOSE_TEXT, "bridge"))
        self.ghidra = _strip_comments(_service_block(COMPOSE_TEXT, "ghidra"))
        self.compose = _strip_comments(COMPOSE_TEXT)

    def test_bridge_uses_ghidra_network_namespace(self) -> None:
        self.assertIn('network_mode: "service:ghidra"', self.bridge)
        # network_mode: service: is incompatible with an explicit networks list.
        self.assertNotRegex(self.bridge, r"(?m)^\s*networks:\s*$")

    def test_bridge_targets_loopback_instance_url(self) -> None:
        self.assertIn("GHIDRA_MCP_URL: http://127.0.0.1:8089", self.bridge)
        self.assertIn("GHIDRA_SERVER_URL: http://127.0.0.1:8089", self.bridge)
        self.assertNotIn("http://ghidra:8089", self.bridge)

    def test_security_properties_preserved(self) -> None:
        # no published host port anywhere in the stack
        self.assertNotRegex(self.compose, r"(?m)^\s*ports:\s*$")
        # loopback-URL validation is not weakened by any bridge env override
        self.assertNotIn("GHIDRA_MCP_ALLOW", self.bridge)
        self.assertIn('GHIDRA_MCP_ALLOW_SCRIPTS: "0"', self.ghidra)
        self.assertIn(":/inputs:ro", self.ghidra)
        for block in (self.bridge, self.ghidra):
            self.assertIn("cap_drop:", block)
            self.assertIn("- ALL", block)
            self.assertIn("no-new-privileges:true", block)
        self.assertIn("read_only: true", self.bridge)
        self.assertRegex(COMPOSE_TEXT, r"backend:\s*\n\s*internal: true")
        # no docker socket / repo / home / credential mount
        self.assertNotIn("/var/run/docker.sock", COMPOSE_TEXT)
        self.assertNotIn("${HOME}", COMPOSE_TEXT)


class GhidraRuntimeRootWorktreeTest(unittest.TestCase):
    """SEG-007-T162 Scope §3: the canonical ignored runtime/input root must be
    derived from the Git common repository root, so a linked ``git worktree``
    resolves the exact same ``.tools/ghidra`` directory as the base checkout
    (and therefore the same token and read-only ``/inputs`` mount)."""

    def _runtime_root_from(self, cwd: Path) -> str:
        script = (
            "import importlib.util, pathlib;"
            "s=importlib.util.spec_from_file_location('g', r'%s');"
            "m=importlib.util.module_from_spec(s); s.loader.exec_module(m);"
            "print(m.DEFAULT_RUNTIME_ROOT)" % (cwd / "tools" / "ghidra.py")
        )
        result = subprocess.run(
            ["python3", "-c", script], check=True, text=True, capture_output=True, cwd=str(cwd)
        )
        return result.stdout.strip()

    def test_linked_worktree_resolves_same_runtime_root(self) -> None:
        if not (PROJECT_ROOT / ".git").exists():
            self.skipTest("not a primary git checkout")
        base_root = self._runtime_root_from(PROJECT_ROOT)
        with tempfile.TemporaryDirectory() as temporary:
            linked = Path(temporary) / "linked"
            try:
                subprocess.run(
                    ["git", "worktree", "add", "--detach", str(linked)],
                    check=True,
                    text=True,
                    capture_output=True,
                    cwd=str(PROJECT_ROOT),
                )
            except subprocess.CalledProcessError as error:  # pragma: no cover
                self.skipTest(f"git worktree unavailable: {error.stderr}")
            try:
                # git worktree add checks out committed HEAD; mirror the current
                # working-tree tool so the resolution logic under test is what runs.
                (linked / "tools").mkdir(parents=True, exist_ok=True)
                (linked / "tools" / "ghidra.py").write_text(
                    (PROJECT_ROOT / "tools" / "ghidra.py").read_text(encoding="utf-8"),
                    encoding="utf-8",
                )
                linked_root = self._runtime_root_from(linked)
                self.assertEqual(base_root, linked_root)
                self.assertTrue(linked_root.endswith(os.path.join(".tools", "ghidra")))
            finally:
                subprocess.run(
                    ["git", "worktree", "remove", "--force", str(linked)],
                    check=False,
                    text=True,
                    capture_output=True,
                    cwd=str(PROJECT_ROOT),
                )

    def test_runtime_root_falls_back_without_git(self) -> None:
        with patch.object(
            GHIDRA_TOOL.subprocess, "run", side_effect=FileNotFoundError("git")
        ):
            self.assertEqual(GHIDRA_TOOL._main_worktree_root(), GHIDRA_TOOL.PROJECT_ROOT)


SHA_A = "a" * 64
SHA_B = "b" * 64


def _candidate(sha: str, address: int, *, tool_version: str = "SEG-007-T179-ADR-0025") -> dict:
    return {
        "kind": "code_entry_candidate",
        "rom_sha256": sha,
        "address": address,
        "provenance": {
            "tool": "ghidra",
            "tool_version": tool_version,
            "timestamp": "1970-01-01T00:00:00Z",
            "human_reviewed": False,
        },
    }


def _table(sha: str, *, base_address: int = 0x0B10, stride: int = 2) -> dict:
    return {
        "kind": "logical_table_descriptor",
        "rom_sha256": sha,
        "base_address": base_address,
        "entry_width_bytes": 2,
        "stride_bytes": stride,
        "entry_count": 13,
        "provenance": {
            "tool": "ghidra",
            "tool_version": "SEG-007-T164-correction-cycle-2",
            "timestamp": "2026-09-04T00:00:00Z",
            "human_reviewed": True,
        },
    }


def _code_pointer_table(sha: str, *, base_address: int = 0x4000,
                        entry_count: int = 3) -> dict:
    return {
        "kind": "code_pointer_table_descriptor",
        "rom_sha256": sha,
        "base_address": base_address,
        "source": "immutable_cartridge",
        "pointer_type": "absolute_code_address",
        "entry_width_bytes": 4,
        "stride_bytes": 4,
        "entry_count": entry_count,
        "provenance": {
            "tool": "human-review",
            "tool_version": "synthetic-1",
            "timestamp": "2026-09-10T00:00:00Z",
            "human_reviewed": True,
        },
    }


class GhidraComposeHintsTest(unittest.TestCase):
    """ADR-0025: offline Ghidra analysis AUGMENTS, never REPLACES, the
    pre-existing ROM-bound heterogeneous structured hints."""

    def _compose(self, root: Path, candidates: list, base: list, **kw) -> list:
        cand_path = root / "cand.json"
        base_path = root / "base.json"
        out_path = root / "composed.json"
        cand_path.write_text(json.dumps(candidates), encoding="utf-8")
        base_path.write_text(json.dumps(base), encoding="utf-8")
        GHIDRA_TOOL.compose_hints(
            cand_path, base_path, None, kw.get("sha", SHA_A), out_path
        )
        return json.loads(out_path.read_text(encoding="utf-8"))

    def _compose_raw(self, root: Path, candidates: str, base: str) -> None:
        cand_path = root / "cand.json"
        base_path = root / "base.json"
        cand_path.write_text(candidates, encoding="utf-8")
        base_path.write_text(base, encoding="utf-8")
        GHIDRA_TOOL.compose_hints(
            cand_path, base_path, None, SHA_A, root / "composed.json"
        )

    def test_union_preserves_table_descriptor_and_dedups_candidates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            table = _table(SHA_A)
            composed = self._compose(
                root,
                candidates=[_candidate(SHA_A, 0x1000), _candidate(SHA_A, 0x2000)],
                base=[table, _candidate(SHA_A, 0x2000, tool_version="6.0.0"),
                      _candidate(SHA_A, 0x3000, tool_version="6.0.0")],
            )
            kinds = [r["kind"] for r in composed]
            self.assertEqual(kinds.count("logical_table_descriptor"), 1)
            # the structured record survives byte-for-byte (every field intact)
            self.assertIn(table, composed)
            addrs = sorted(r["address"] for r in composed if r["kind"] == "code_entry_candidate")
            self.assertEqual(addrs, [0x1000, 0x2000, 0x3000])
            # candidate dedup by address keeps the Ghidra-export record
            merged_2000 = next(r for r in composed
                               if r["kind"] == "code_entry_candidate" and r["address"] == 0x2000)
            self.assertEqual(merged_2000["provenance"]["tool_version"], "SEG-007-T179-ADR-0025")

    def test_union_preserves_both_structured_descriptor_kinds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logical = _table(SHA_A)
            pointers = _code_pointer_table(SHA_A)
            composed = self._compose(
                root, candidates=[_candidate(SHA_A, 0x1000)],
                base=[logical, pointers, _candidate(SHA_A, 0x2000)],
            )
            self.assertIn(logical, composed)
            self.assertIn(pointers, composed)
            self.assertEqual(
                sorted(r["address"] for r in composed
                       if r["kind"] == "code_entry_candidate"),
                [0x1000, 0x2000],
            )

    def test_code_pointer_descriptor_conflict_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                self._compose(
                    root, candidates=[_candidate(SHA_A, 0x1000)],
                    base=[_code_pointer_table(SHA_A, entry_count=2),
                          _code_pointer_table(SHA_A, entry_count=3)],
                )

    def test_descriptor_numeric_spellings_normalize_before_conflict_checks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            numeric = _code_pointer_table(SHA_A, base_address=0x4000, entry_count=3)
            numeric["entry_count"] = 3.0
            spelled = json.loads(json.dumps(numeric))
            spelled["base_address"] = "0x4000"
            spelled["entry_width_bytes"] = "4"
            spelled["stride_bytes"] = "0x4"
            spelled["entry_count"] = "03"
            logical_numeric = _table(SHA_A)
            logical_spelled = json.loads(json.dumps(logical_numeric))
            logical_spelled["base_address"] = "0x0B10"
            logical_spelled["entry_width_bytes"] = "02"
            logical_spelled["stride_bytes"] = "2"
            logical_spelled["entry_count"] = "0xD"
            composed = self._compose(
                root, [_candidate(SHA_A, 0x1000)],
                [numeric, spelled, logical_numeric, logical_spelled],
            )
            descriptors = [r for r in composed if r["kind"] == "code_pointer_table_descriptor"]
            self.assertEqual(len(descriptors), 1)
            self.assertEqual(descriptors[0]["entry_count"], 3)
            self.assertIsInstance(descriptors[0]["entry_count"], int)
            logical_descriptors = [r for r in composed if r["kind"] == "logical_table_descriptor"]
            self.assertEqual(logical_descriptors, [logical_numeric])

            conflicting = json.loads(json.dumps(spelled))
            conflicting["entry_count"] = "0x4"
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                self._compose(root, [_candidate(SHA_A, 0x1000)], [numeric, conflicting])

    def test_malformed_recognized_descriptors_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            malformed = [
                {**_table(SHA_A), "entry_count": "2.0"},
                {**_table(SHA_A), "stride_bytes": 0},
                {**_code_pointer_table(SHA_A), "entry_width_bytes": "0x2"},
                {**_code_pointer_table(SHA_A), "entry_count": 257},
                {**_code_pointer_table(SHA_A), "provenance": {"human_reviewed": True}},
            ]
            for record in malformed:
                with self.subTest(record=record):
                    with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                        self._compose(root, [_candidate(SHA_A, 0x1000)], [record])

    def test_duplicate_keys_in_both_compose_inputs_fail_closed(self) -> None:
        candidate = json.dumps(_candidate(SHA_A, 0x1000), separators=(",", ":"))
        table = json.dumps(_table(SHA_A), separators=(",", ":"))
        candidate_top = candidate.replace('"address":4096', '"address":4096,"address":8192')
        candidate_nested = candidate.replace('"tool":"ghidra"', '"tool":"ghidra","tool":"other"')
        base_top = table.replace('"entry_count":13', '"entry_count":13,"entry_count":14')
        base_nested = table.replace('"tool":"ghidra"', '"tool":"ghidra","tool":"other"')
        for candidates, base in (
            (f"[{candidate_top}]", f"[{table}]"),
            (f"[{candidate_nested}]", f"[{table}]"),
            (f"[{candidate}]", f"[{base_top}]"),
            (f"[{candidate}]", f"[{base_nested}]"),
        ):
            with self.subTest(candidates=candidates, base=base):
                with tempfile.TemporaryDirectory() as tmp:
                    with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                        self._compose_raw(Path(tmp), candidates, base)

    def test_nonstandard_numeric_constants_in_both_inputs_fail_closed(self) -> None:
        candidate = json.dumps(_candidate(SHA_A, 0x1000), separators=(",", ":"))
        table = json.dumps(_table(SHA_A), separators=(",", ":"))
        for candidates, base in (
            (candidate.replace("4096", "NaN"), table),
            (candidate, table.replace("13", "Infinity")),
        ):
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                    self._compose_raw(Path(tmp), f"[{candidates}]", f"[{base}]")

    def test_compose_nesting_is_bounded_before_recursive_json_loading(self) -> None:
        candidate = json.dumps(_candidate(SHA_A, 0x1000), separators=(",", ":"))
        table = json.dumps(_table(SHA_A), separators=(",", ":"))

        def nested(depth: int, objects: bool, truncated: bool = False) -> str:
            prefix = '{"value":' * depth if objects else "[" * depth
            suffix = "" if truncated else (("}" if objects else "]") * depth)
            return prefix + "null" + suffix

        def with_extra(record: str, value: str) -> str:
            return record[:-1] + ',"extra":' + value + "}"

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._compose_raw(
                root,
                f"[{with_extra(candidate, nested(16, False))}]",
                f"[{with_extra(table, nested(16, True))}]",
            )

        cases = (
            (with_extra(candidate, nested(4096, False)), table, "candidate arrays"),
            (candidate, with_extra(table, nested(4096, True)), "base objects"),
            (with_extra(candidate, nested(4096, False, True)), table,
             "truncated candidate arrays"),
            (candidate, with_extra(table, nested(4096, True, True)),
             "truncated base objects"),
        )
        for candidates, base, label in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(
                    GHIDRA_TOOL.GhidraToolError, "JSON nesting depth exceeds limit"
                ):
                    self._compose_raw(Path(tmp), f"[{candidates}]", f"[{base}]")

    def test_code_pointer_provenance_variants_are_preserved_without_conflict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = _code_pointer_table(SHA_A)
            second = json.loads(json.dumps(first))
            second["provenance"]["tool"] = "independent-review"
            composed = self._compose(
                root, candidates=[_candidate(SHA_A, 0x1000)],
                base=[second, first, first],
            )
            descriptors = [record for record in composed
                           if record["kind"] == "code_pointer_table_descriptor"]
            self.assertEqual(len(descriptors), 2)
            self.assertEqual(
                descriptors,
                sorted(descriptors, key=lambda record: json.dumps(
                    record, sort_keys=True, separators=(",", ":"))),
            )
            self.assertEqual(
                {record["provenance"]["tool"] for record in descriptors},
                {"human-review", "independent-review"},
            )

    def test_composition_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand = [_candidate(SHA_A, a) for a in (0x30, 0x10, 0x20)]
            base = [_table(SHA_A), _candidate(SHA_A, 0x40)]
            (root / "c.json").write_text(json.dumps(cand), encoding="utf-8")
            (root / "b.json").write_text(json.dumps(base), encoding="utf-8")
            first = root / "first.json"
            second = root / "second.json"
            GHIDRA_TOOL.compose_hints(root / "c.json", root / "b.json", None, SHA_A, first)
            GHIDRA_TOOL.compose_hints(root / "c.json", root / "b.json", None, SHA_A, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertTrue(first.read_bytes().endswith(b"}]\n"))

    def test_fails_closed_on_incompatible_rom_identity(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                self._compose(root, candidates=[_candidate(SHA_B, 0x1000)],
                              base=[_table(SHA_A)])
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                self._compose(root, candidates=[_candidate(SHA_A, 0x1000)],
                              base=[_table(SHA_B)])

    def test_fails_closed_on_conflicting_structured_records(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                self._compose(
                    root,
                    candidates=[_candidate(SHA_A, 0x1000)],
                    base=[_table(SHA_A, base_address=0x0B10, stride=2),
                          _table(SHA_A, base_address=0x0B10, stride=4)],
                )

    def test_rejects_non_candidate_records_in_the_ghidra_export(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                self._compose(root, candidates=[_table(SHA_A)], base=[_table(SHA_A)])

    def test_refuses_to_write_over_a_canonical_per_rom_hints_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            hints_dir = root / "analysis-hints"
            hints_dir.mkdir()
            (root / "cand.json").write_text(json.dumps([_candidate(SHA_A, 0x10)]), encoding="utf-8")
            base = hints_dir / f"{SHA_A}.json"
            base.write_text(json.dumps([_table(SHA_A)]), encoding="utf-8")
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                GHIDRA_TOOL.compose_hints(
                    root / "cand.json", base, None, SHA_A, hints_dir / f"{SHA_A}.json"
                )

    def test_address_table_candidates_union_alongside_other_structured_kinds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            address_table_path = root / "address-tables.json"
            out_path = root / "composed.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(
                json.dumps([_table(SHA_A), _code_pointer_table(SHA_A)]), encoding="utf-8"
            )
            address_table_path.write_text(
                json.dumps([_address_table_candidate(SHA_A)]), encoding="utf-8"
            )
            GHIDRA_TOOL.compose_hints(
                cand_path, base_path, None, SHA_A, out_path,
                address_table_candidates_path=address_table_path,
            )
            composed = json.loads(out_path.read_text(encoding="utf-8"))
            kinds = [record["kind"] for record in composed]
            self.assertEqual(
                sorted(kinds),
                sorted(["code_entry_candidate", "logical_table_descriptor",
                        "code_pointer_table_descriptor", "address_table_candidate"]),
            )
            promoted = next(r for r in composed if r["kind"] == "address_table_candidate")
            self.assertEqual(promoted["base_address"], 0x0B80)
            self.assertFalse(promoted["provenance"]["human_reviewed"])

    def test_address_table_candidates_non_candidate_record_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            address_table_path = root / "address-tables.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([_table(SHA_A)]), encoding="utf-8")
            address_table_path.write_text(json.dumps([_table(SHA_A)]), encoding="utf-8")
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                GHIDRA_TOOL.compose_hints(
                    cand_path, base_path, None, SHA_A, root / "composed.json",
                    address_table_candidates_path=address_table_path,
                )

    def test_address_table_candidate_malformed_fields_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            malformed = [
                {**_address_table_candidate(SHA_A), "entry_width_bytes": 2},
                {**_address_table_candidate(SHA_A), "stride_bytes": 8},
                {**_address_table_candidate(SHA_A), "entry_count": 257},
                {**_address_table_candidate(SHA_A), "rom_sha256": SHA_B},
            ]
            for record in malformed:
                with self.subTest(record=record):
                    with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                        self._compose(
                            root, candidates=[_candidate(SHA_A, 0x1000)], base=[record],
                        )

    def test_address_table_candidate_does_not_require_human_review(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            composed = self._compose(
                root, candidates=[_candidate(SHA_A, 0x1000)],
                base=[_address_table_candidate(SHA_A)],
            )
            promoted = [r for r in composed if r["kind"] == "address_table_candidate"]
            self.assertEqual(len(promoted), 1)
            self.assertFalse(promoted[0]["provenance"]["human_reviewed"])

    # SEG-007-T206 / ADR-0034: platforms/genesis/compat/<rom-sha256>.json union.

    def test_compat_genesis_descriptor_unions_like_base_hints(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            compat_path = root / "compat.json"
            out_path = root / "composed.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([]), encoding="utf-8")
            compat_table = _table(SHA_A, base_address=0x2000)
            compat_path.write_text(json.dumps([compat_table]), encoding="utf-8")
            GHIDRA_TOOL.compose_hints(
                cand_path, base_path, None, SHA_A, out_path, compat_genesis_path=compat_path,
            )
            composed = json.loads(out_path.read_text(encoding="utf-8"))
            self.assertIn(compat_table, composed)

    def test_compat_genesis_two_entry_file_unions_both_descriptors_without_interference(
        self,
    ) -> None:
        # Mirrors the real committed platforms/genesis/compat/<rom-sha256>.json for the
        # authorized Sonic ROM after the SEG-007-T164 migration: one entry is
        # this task's own entry_count=2 assertion, the second is the
        # migrated SEG-007-T164 entry_count=13 assertion (a distinct
        # base_address). Both must reach the composed output identically and
        # independently -- neither one may shadow, merge into, or otherwise
        # interfere with the other.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            compat_path = root / "compat.json"
            out_path = root / "composed.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([]), encoding="utf-8")
            own_table = _table(SHA_A, base_address=0x2000)
            migrated_table = _table(SHA_A, base_address=0x0B72)
            migrated_table["entry_count"] = 13
            migrated_table["provenance"] = {
                "tool": "ghidra",
                "tool_version": "SEG-007-T164-correction-cycle-2",
                "timestamp": "2026-09-04T00:00:00Z",
                "human_reviewed": False,
            }
            compat_path.write_text(
                json.dumps([own_table, migrated_table]), encoding="utf-8",
            )
            GHIDRA_TOOL.compose_hints(
                cand_path, base_path, None, SHA_A, out_path, compat_genesis_path=compat_path,
            )
            composed = json.loads(out_path.read_text(encoding="utf-8"))
            self.assertIn(own_table, composed)
            self.assertIn(migrated_table, composed)
            descriptors = [r for r in composed if r["kind"] == "logical_table_descriptor"]
            self.assertEqual(len(descriptors), 2)
            self.assertEqual(
                {d["base_address"] for d in descriptors},
                {own_table["base_address"], migrated_table["base_address"]},
            )

    def test_compat_genesis_absent_file_is_a_no_op(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            composed = self._compose(
                root, candidates=[_candidate(SHA_A, 0x1000)], base=[_table(SHA_A)],
            )
            # No compat_genesis_path given, default resolves to a non-existent
            # path under this worktree's own platforms/genesis/compat/ (PROJECT_ROOT) --
            # no error, no extra record beyond what base/candidates already
            # supplied.
            self.assertEqual(
                sorted(r["kind"] for r in composed),
                sorted(["code_entry_candidate", "logical_table_descriptor"]),
            )

    def test_compat_genesis_empty_array_is_a_no_op(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            compat_path = root / "compat.json"
            out_path = root / "composed.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([_table(SHA_A)]), encoding="utf-8")
            compat_path.write_text("[]", encoding="utf-8")
            GHIDRA_TOOL.compose_hints(
                cand_path, base_path, None, SHA_A, out_path, compat_genesis_path=compat_path,
            )
            composed = json.loads(out_path.read_text(encoding="utf-8"))
            self.assertEqual(
                sorted(r["kind"] for r in composed),
                sorted(["code_entry_candidate", "logical_table_descriptor"]),
            )

    def test_compat_genesis_malformed_top_level_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            compat_path = root / "compat.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([]), encoding="utf-8")
            compat_path.write_text(json.dumps(_table(SHA_A)), encoding="utf-8")  # object, not array
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                GHIDRA_TOOL.compose_hints(
                    cand_path, base_path, None, SHA_A, root / "composed.json",
                    compat_genesis_path=compat_path,
                )

    def test_compat_genesis_duplicate_identity_conflict_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            compat_path = root / "compat.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([_table(SHA_A, base_address=0x2000)]), encoding="utf-8")
            conflicting = _table(SHA_A, base_address=0x2000)
            conflicting["entry_count"] = 99
            compat_path.write_text(json.dumps([conflicting]), encoding="utf-8")
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                GHIDRA_TOOL.compose_hints(
                    cand_path, base_path, None, SHA_A, root / "composed.json",
                    compat_genesis_path=compat_path,
                )

    def test_compat_genesis_wrong_rom_identity_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cand_path = root / "cand.json"
            base_path = root / "base.json"
            compat_path = root / "compat.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([]), encoding="utf-8")
            compat_path.write_text(json.dumps([_table(SHA_B, base_address=0x2000)]), encoding="utf-8")
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                GHIDRA_TOOL.compose_hints(
                    cand_path, base_path, None, SHA_A, root / "composed.json",
                    compat_genesis_path=compat_path,
                )

    def test_compat_genesis_default_resolves_against_current_worktree_not_main(self) -> None:
        # SEG-007-T206 correction: platforms/genesis/compat/ is a git-tracked, per-branch
        # artifact (unlike the ignored, intentionally cross-worktree-shared
        # .tools/analysis-hints/ cache). Its default path must resolve against
        # THIS invocation's own current worktree (PROJECT_ROOT), never the
        # shared main-worktree root -- otherwise composing hints from a task's
        # own worktree (this project's standard delivery pattern) would
        # silently miss that task's own not-yet-merged compat file. Simulate
        # two distinct worktree roots: the compat file exists ONLY under the
        # "current worktree" fixture, never under the "main worktree" fixture.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            current_worktree = root / "current-worktree"
            main_worktree = root / "main-worktree"
            (current_worktree / "platforms" / "genesis" / "compat").mkdir(parents=True)
            main_worktree.mkdir(parents=True)

            cand_path = current_worktree / "cand.json"
            base_path = current_worktree / "base.json"
            out_path = current_worktree / "composed.json"
            cand_path.write_text(json.dumps([_candidate(SHA_A, 0x1000)]), encoding="utf-8")
            base_path.write_text(json.dumps([]), encoding="utf-8")
            compat_table = _table(SHA_A, base_address=0x2000)
            (current_worktree / "platforms" / "genesis" / "compat" / f"{SHA_A}.json").write_text(
                json.dumps([compat_table]), encoding="utf-8",
            )

            with patch.object(GHIDRA_TOOL, "PROJECT_ROOT", current_worktree), \
                    patch.object(GHIDRA_TOOL, "_main_worktree_root", return_value=main_worktree):
                # No compat_genesis_path argument: exercise default resolution.
                GHIDRA_TOOL.compose_hints(cand_path, base_path, None, SHA_A, out_path)

            composed = json.loads(out_path.read_text(encoding="utf-8"))
            self.assertIn(compat_table, composed)

    def test_refuses_to_write_over_canonical_compat_genesis_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            compat_dir = root / "platforms" / "genesis" / "compat"
            compat_dir.mkdir(parents=True)
            (root / "cand.json").write_text(json.dumps([_candidate(SHA_A, 0x10)]), encoding="utf-8")
            base = root / "base.json"
            base.write_text(json.dumps([_table(SHA_A)]), encoding="utf-8")
            compat_file = compat_dir / f"{SHA_A}.json"
            compat_file.write_text(json.dumps([_table(SHA_A, base_address=0x2000)]), encoding="utf-8")
            with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                GHIDRA_TOOL.compose_hints(
                    root / "cand.json", base, None, SHA_A, compat_file,
                    compat_genesis_path=compat_file,
                )


def _address_table_candidate(sha: str, *, base_address: int = 0x0B80, entry_count: int = 2) -> dict:
    return {
        "kind": "address_table_candidate",
        "rom_sha256": sha,
        "base_address": base_address,
        "entry_width_bytes": 4,
        "stride_bytes": 4,
        "entry_count": entry_count,
        "provenance": {
            "tool": "ghidra",
            "tool_version": "SEG-007-T204-ADR-0033",
            "timestamp": "1970-01-01T00:00:00Z",
            "human_reviewed": False,
        },
    }


class GhidraCanonicalizeAddressTableCandidatesTest(unittest.TestCase):
    """SEG-007-T204 / ADR-0033: ``ExportAddressTableCandidates.java`` output
    re-serialization -- raw, explicitly non-authoritative material, never
    itself an ADR-0023 descriptor."""

    def test_valid_export_is_sorted_deduplicated_and_deterministic(self) -> None:
        raw = json.dumps([
            _address_table_candidate(SHA_A, base_address=0x2000),
            _address_table_candidate(SHA_A, base_address=0x1000),
            _address_table_candidate(SHA_A, base_address=0x1000, entry_count=2),
        ])
        canonical = GHIDRA_TOOL.canonicalize_address_table_candidates(raw, SHA_A)
        records = json.loads(canonical)
        self.assertEqual([r["base_address"] for r in records], [0x1000, 0x2000])
        self.assertTrue(canonical.endswith("]\n"))
        # A second, freshly-serialised run is byte-identical.
        self.assertEqual(
            canonical,
            GHIDRA_TOOL.canonicalize_address_table_candidates(raw, SHA_A),
        )

    def test_rejects_wrong_kind(self) -> None:
        raw = json.dumps([_table(SHA_A)])
        with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
            GHIDRA_TOOL.canonicalize_address_table_candidates(raw, SHA_A)

    def test_rejects_rom_hash_mismatch(self) -> None:
        raw = json.dumps([_address_table_candidate(SHA_A)])
        with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
            GHIDRA_TOOL.canonicalize_address_table_candidates(raw, SHA_B)

    def test_rejects_invalid_width_stride_or_count(self) -> None:
        for override in ({"entry_width_bytes": 2}, {"stride_bytes": 8}, {"entry_count": 0},
                         {"entry_count": 257}):
            with self.subTest(override=override):
                raw = json.dumps([{**_address_table_candidate(SHA_A), **override}])
                with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
                    GHIDRA_TOOL.canonicalize_address_table_candidates(raw, SHA_A)

    def test_rejects_non_array_top_level(self) -> None:
        with self.assertRaises(GHIDRA_TOOL.GhidraToolError):
            GHIDRA_TOOL.canonicalize_address_table_candidates(
                json.dumps(_address_table_candidate(SHA_A)), SHA_A)


if __name__ == "__main__":
    unittest.main()
