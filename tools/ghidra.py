#!/usr/bin/env python3
"""Manage the project-local, containerized Ghidra MCP stack."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
from decimal import Decimal
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
COMPOSE_FILE = PROJECT_ROOT / "tools" / "ghidra" / "compose.yaml"


def _main_worktree_root() -> Path:
    """Resolve the top-level of the main working tree (the Git common repo root).

    A linked ``git worktree`` has its own ``parents[1]`` but shares one running
    Ghidra stack, whose token and read-only ``/inputs`` mount live under the main
    checkout. Deriving the runtime root from ``git rev-parse --git-common-dir``
    keeps every linked worktree pointing at that single canonical location. Falls
    back to ``parents[1]`` when git is unavailable or the path cannot be resolved.
    """

    try:
        result = subprocess.run(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            check=True,
            text=True,
            capture_output=True,
            cwd=str(PROJECT_ROOT),
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return PROJECT_ROOT
    common_dir = Path(result.stdout.strip())
    if not common_dir.is_absolute() or not common_dir.exists():
        return PROJECT_ROOT
    # <main-worktree>/.git  ->  <main-worktree>
    return common_dir.parent


DEFAULT_RUNTIME_ROOT = _main_worktree_root() / ".tools" / "ghidra"
PROJECT_NAME = "segarecomp-ghidra"
CODE_POINTER_TABLE_MAX_ENTRIES = 256
STRICT_JSON_MAX_NESTING_DEPTH = 64


class GhidraToolError(Exception):
    """A deterministic, user-facing lifecycle error."""


def _strict_json_load(path: Path) -> object:
    """Load untrusted interchange JSON without duplicate-name or NaN authority."""

    def reject_duplicates(pairs: list[tuple[str, object]]) -> dict:
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate object key {key!r}")
            result[key] = value
        return result

    def reject_constant(value: str) -> object:
        raise ValueError(f"invalid JSON numeric constant {value!r}")

    def reject_excessive_nesting(text: str) -> None:
        depth = 0
        in_string = False
        escaped = False
        for character in text:
            if in_string:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == '"':
                    in_string = False
                continue
            if character == '"':
                in_string = True
            elif character in "[{":
                depth += 1
                if depth > STRICT_JSON_MAX_NESTING_DEPTH:
                    raise ValueError("JSON nesting depth exceeds limit")
            elif character in "]}" and depth > 0:
                depth -= 1

    try:
        text = path.read_text(encoding="utf-8")
        reject_excessive_nesting(text)
        return json.loads(
            text,
            object_pairs_hook=reject_duplicates,
            parse_float=Decimal,
            parse_constant=reject_constant,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError, RecursionError) as exc:
        raise GhidraToolError(f"invalid JSON in {path.name}: {exc}") from exc


def ensure_runtime(runtime_root: Path) -> Path:
    runtime_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    runtime_root.chmod(0o700)
    input_root = runtime_root / "input"
    input_root.mkdir(mode=0o700, exist_ok=True)

    env_file = runtime_root / "stack.env"
    if not env_file.exists():
        token = secrets.token_hex(32)
        env_file.write_text(
            f"GHIDRA_MCP_AUTH_TOKEN={token}\nGHIDRA_INPUT_DIR={input_root.resolve()}\n",
            encoding="utf-8",
        )
        env_file.chmod(stat.S_IRUSR | stat.S_IWUSR)
    else:
        mode = stat.S_IMODE(env_file.stat().st_mode)
        # POSIX mode bits do not model Windows ACLs; the 0600 rule is POSIX-only.
        if os.name != "nt" and mode & (stat.S_IRWXG | stat.S_IRWXO):
            raise GhidraToolError(f"secret file permissions must be 0600: {env_file}")
        values = parse_env(env_file)
        token = values.get("GHIDRA_MCP_AUTH_TOKEN", "")
        if len(token) < 64:
            raise GhidraToolError(f"invalid GHIDRA_MCP_AUTH_TOKEN in {env_file}")
        expected_input = str(input_root.resolve())
        if values.get("GHIDRA_INPUT_DIR") != expected_input:
            values["GHIDRA_INPUT_DIR"] = expected_input
            env_file.write_text(
                "".join(f"{key}={values[key]}\n" for key in sorted(values)), encoding="utf-8"
            )
            env_file.chmod(stat.S_IRUSR | stat.S_IWUSR)
    return env_file


def parse_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key:
            raise GhidraToolError(f"invalid environment line in {path}")
        values[key] = value
    return values


def compose_command(runtime_root: Path, *arguments: str) -> list[str]:
    env_file = ensure_runtime(runtime_root)
    return [
        "docker",
        "compose",
        "--project-name",
        PROJECT_NAME,
        "--env-file",
        str(env_file),
        "--file",
        str(COMPOSE_FILE),
        *arguments,
    ]


def run(runtime_root: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(compose_command(runtime_root, *arguments), check=check, text=True)
    except FileNotFoundError as error:
        raise GhidraToolError("Docker is not installed or not on PATH") from error
    except subprocess.CalledProcessError as error:
        raise GhidraToolError(f"Docker Compose failed with status {error.returncode}") from error


def running_services(runtime_root: Path) -> set[str]:
    result = subprocess.run(
        compose_command(runtime_root, "ps", "--status", "running", "--format", "json"),
        check=True,
        text=True,
        capture_output=True,
    )
    if not result.stdout.strip():
        return set()

    services: set[str] = set()
    decoder = json.JSONDecoder()
    position = 0
    while position < len(result.stdout):
        while position < len(result.stdout) and result.stdout[position].isspace():
            position += 1
        if position == len(result.stdout):
            break
        payload, position = decoder.raw_decode(result.stdout, position)
        values = payload if isinstance(payload, list) else [payload]
        for value in values:
            if isinstance(value, dict) and isinstance(value.get("Service"), str):
                services.add(value["Service"])
    return services


def exec_stdio(runtime_root: Path) -> None:
    services = running_services(runtime_root)
    missing = sorted({"ghidra", "bridge"} - services)
    if missing:
        raise GhidraToolError(f"stack is not running ({', '.join(missing)}); run 'tools/ghidra.py up'")
    command = compose_command(
        runtime_root,
        "exec",
        "--no-TTY",
        "bridge",
        "bridge-mcp-ghidra",
        "--transport",
        "stdio",
        "--no-lazy",
    )
    os.execvp(command[0], command)


ANALYSIS_TIMEOUT_PER_FILE = "1200"
ANALYSIS_SCRIPT = "ExportCodeEntryCandidates"
# SEG-007-T204 / ADR-0033: sibling raw, explicitly non-authoritative export of
# Ghidra's own "Create Address Tables" analyzer output. Runs as a second
# -postScript in the same headless invocation; never replaces or modifies
# ANALYSIS_SCRIPT's own candidates.json output.
ADDRESS_TABLE_ANALYSIS_SCRIPT = "ExportAddressTableCandidates"
# SEG-007-T179 / ADR-0025: the pinned deterministic analyzer recipe itself lives
# in tools/ghidra/scripts/PinAnalysisRecipe.java (run as -preScript) so it is
# applied inside the pinned image regardless of host Ghidra state.


def canonicalize_candidates(raw_text: str, expected_sha: str) -> str:
    """Re-serialise the script output deterministically.

    Records are validated to the ADR-0024 ``code_entry_candidate`` shape,
    sorted by address, de-duplicated, and emitted with a stable key order and a
    trailing newline so two clean runs are byte-identical.
    """

    payload = json.loads(raw_text)
    if not isinstance(payload, list):
        raise GhidraToolError("analysis script did not emit a JSON array")
    seen: set[int] = set()
    records: list[dict] = []
    for entry in payload:
        if not isinstance(entry, dict) or entry.get("kind") != "code_entry_candidate":
            raise GhidraToolError("unexpected record kind in analysis output")
        if entry.get("rom_sha256") != expected_sha:
            raise GhidraToolError("analysis record ROM hash mismatch")
        address = entry.get("address")
        if not isinstance(address, int) or isinstance(address, bool) or address < 0 or address % 2 != 0:
            raise GhidraToolError("invalid candidate address in analysis output")
        if address in seen:
            continue
        seen.add(address)
        provenance = entry.get("provenance")
        if not isinstance(provenance, dict):
            raise GhidraToolError("analysis record missing provenance")
        records.append(
            {
                "kind": "code_entry_candidate",
                "rom_sha256": expected_sha,
                "address": address,
                "provenance": provenance,
            }
        )
    records.sort(key=lambda item: item["address"])
    return json.dumps(records, sort_keys=True, separators=(",", ":")) + "\n"


def canonicalize_address_table_candidates(raw_text: str, expected_sha: str) -> str:
    """Re-serialise ``ExportAddressTableCandidates.java`` output deterministically.

    Records are validated to the SEG-007-T204 / ADR-0033 ``address_table_candidate``
    shape, sorted by base address, de-duplicated, and emitted with a stable key
    order and a trailing newline so two clean runs are byte-identical. This is raw,
    explicitly non-authoritative material -- never itself an ADR-0023 descriptor.
    """

    payload = json.loads(raw_text)
    if not isinstance(payload, list):
        raise GhidraToolError("address-table analysis script did not emit a JSON array")
    seen: set[int] = set()
    records: list[dict] = []
    for entry in payload:
        if not isinstance(entry, dict) or entry.get("kind") != "address_table_candidate":
            raise GhidraToolError("unexpected record kind in address-table analysis output")
        if entry.get("rom_sha256") != expected_sha:
            raise GhidraToolError("address-table analysis record ROM hash mismatch")
        base_address = entry.get("base_address")
        if not isinstance(base_address, int) or isinstance(base_address, bool) or base_address < 0:
            raise GhidraToolError("invalid address_table_candidate base_address")
        entry_width_bytes = entry.get("entry_width_bytes")
        stride_bytes = entry.get("stride_bytes")
        entry_count = entry.get("entry_count")
        if (entry_width_bytes != 4 or stride_bytes != 4 or
                not isinstance(entry_count, int) or isinstance(entry_count, bool) or
                entry_count < 1 or entry_count > CODE_POINTER_TABLE_MAX_ENTRIES):
            raise GhidraToolError("invalid address_table_candidate width/stride/count")
        if base_address in seen:
            continue
        seen.add(base_address)
        provenance = entry.get("provenance")
        if not isinstance(provenance, dict):
            raise GhidraToolError("address-table analysis record missing provenance")
        records.append(
            {
                "kind": "address_table_candidate",
                "rom_sha256": expected_sha,
                "base_address": base_address,
                "entry_width_bytes": 4,
                "stride_bytes": 4,
                "entry_count": entry_count,
                "provenance": provenance,
            }
        )
    records.sort(key=lambda item: item["base_address"])
    return json.dumps(records, sort_keys=True, separators=(",", ":")) + "\n"


def analyze(runtime_root: Path, rom_path: Path, output_path: Path,
            address_table_output_path: Path | None = None) -> None:
    if not rom_path.is_file():
        raise GhidraToolError(f"ROM not found: {rom_path}")
    ensure_runtime(runtime_root)
    services = running_services(runtime_root)
    if "ghidra" not in services:
        raise GhidraToolError("ghidra service is not running; run 'tools/ghidra.py up'")
    rom_bytes = rom_path.read_bytes()
    rom_sha = hashlib.sha256(rom_bytes).hexdigest()

    input_root = runtime_root / "input"
    staged = input_root / f"analyze-{rom_sha}.bin"
    # Copy (never move/modify/force-add the authorised commercial image) into the
    # ignored runtime input directory the container mounts read-only.
    staged.write_bytes(rom_bytes)
    staged.chmod(0o600)

    with tempfile.TemporaryDirectory(dir=str(input_root)) as work:
        work_path = Path(work)
        out_dir = work_path / "out"
        out_dir.mkdir()
        container_out = "/analysis-out/candidates.json"
        container_address_table_out = "/analysis-out/address-tables.json"
        analyzer_args = [
            "run", "--rm", "--no-deps",
            "--volume", f"{out_dir}:/analysis-out",
            "--entrypoint", "/opt/ghidra/support/analyzeHeadless",
            "ghidra",
            "/tmp", f"seg_analyze_{rom_sha[:12]}",
            "-import", f"/inputs/{staged.name}",
            "-processor", "68000:BE:32:default",
            "-loader", "BinaryLoader",
            "-loader-baseAddr", "0x0",
            "-max-cpu", "1",
            "-analysisTimeoutPerFile", ANALYSIS_TIMEOUT_PER_FILE,
            "-scriptPath", "/scripts",
            "-preScript", "PinAnalysisRecipe.java",
            "-postScript", ANALYSIS_SCRIPT, rom_sha, container_out,
            # SEG-007-T204 / ADR-0033: sibling non-authoritative raw
            # Create-Address-Tables export, same analyzed image, same
            # headless invocation -- no second analysis pass.
            "-postScript", ADDRESS_TABLE_ANALYSIS_SCRIPT, rom_sha, container_address_table_out,
            "-deleteProject",
        ]
        run(runtime_root, *analyzer_args)
        produced = out_dir / "candidates.json"
        if not produced.is_file():
            raise GhidraToolError("analysis produced no candidate artifact")
        canonical = canonicalize_candidates(produced.read_text(encoding="utf-8"), rom_sha)

        produced_address_tables = out_dir / "address-tables.json"
        if not produced_address_tables.is_file():
            raise GhidraToolError("analysis produced no address-table candidate artifact")
        canonical_address_tables = canonicalize_address_table_candidates(
            produced_address_tables.read_text(encoding="utf-8"), rom_sha)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(canonical, encoding="utf-8", newline="\n")
    if address_table_output_path is None:
        address_table_output_path = output_path.parent / f"{output_path.stem}.address-tables.json"
    address_table_output_path.parent.mkdir(parents=True, exist_ok=True)
    address_table_output_path.write_text(canonical_address_tables, encoding="utf-8", newline="\n")
    try:
        staged.unlink()
    except OSError:
        pass
    count = len(json.loads(canonical))
    address_table_count = len(json.loads(canonical_address_tables))
    print(f"analyze: rom_sha256={rom_sha} candidates={count} "
          f"address_table_candidates={address_table_count} output={output_path} "
          f"address_table_output={address_table_output_path}")


CANONICAL_CANDIDATE_KIND = "code_entry_candidate"


def _canonical_json(value: object) -> str:
    """Deterministic JSON spelling, retaining exact Decimal values from input."""

    if isinstance(value, dict):
        return "{" + ",".join(
            json.dumps(key) + ":" + _canonical_json(value[key]) for key in sorted(value)
        ) + "}"
    if isinstance(value, list):
        return "[" + ",".join(_canonical_json(item) for item in value) + "]"
    if isinstance(value, Decimal):
        if not value.is_finite():
            raise GhidraToolError("non-finite JSON number")
        return str(value)
    return json.dumps(value, separators=(",", ":"), allow_nan=False)


def _canonical_artifact(records: list[dict]) -> str:
    """Serialise a heterogeneous analysis artifact deterministically.

    Uses sorted object keys, compact separators, exact input decimal values,
    and a trailing newline. List order is fixed by the caller. Two runs on
    identical inputs are byte-identical.
    """

    return _canonical_json(records) + "\n"


def _validated_candidate_record(entry: object, expected_sha: str) -> dict:
    """Normalise one ``code_entry_candidate`` to the minimal address-only shape
    ``parse_genesis_external_code_entry_candidates`` consumes. Fail closed on
    anything that is not a well-formed candidate for ``expected_sha``."""

    if not isinstance(entry, dict) or entry.get("kind") != CANONICAL_CANDIDATE_KIND:
        raise GhidraToolError("expected a code_entry_candidate record")
    if entry.get("rom_sha256") != expected_sha:
        raise GhidraToolError("code_entry_candidate ROM hash mismatch")
    address = entry.get("address")
    if not isinstance(address, int) or isinstance(address, bool) or address < 0 or address % 2 != 0:
        raise GhidraToolError("invalid code_entry_candidate address")
    provenance = entry.get("provenance")
    if not isinstance(provenance, dict):
        raise GhidraToolError("code_entry_candidate missing provenance")
    return {
        "kind": CANONICAL_CANDIDATE_KIND,
        "rom_sha256": expected_sha,
        "address": address,
        "provenance": provenance,
    }


def _consumer_uint32(value: object, field: str) -> int:
    """Consumer-equivalent uint32 grammar for recognized descriptor scalars."""

    if isinstance(value, bool):
        raise GhidraToolError(f"invalid {field}")
    if isinstance(value, int):
        result = value
    elif isinstance(value, Decimal):
        if not value.is_finite() or value != value.to_integral_value():
            raise GhidraToolError(f"invalid {field}")
        result = int(value)
    elif isinstance(value, str):
        text = value
        base = 10
        if len(text) > 2 and text[:2] in ("0x", "0X"):
            text = text[2:]
            base = 16
        if not text or any(c not in ("0123456789" if base == 10 else
                                     "0123456789abcdefABCDEF") for c in text):
            raise GhidraToolError(f"invalid {field}")
        result = int(text, base)
    else:
        raise GhidraToolError(f"invalid {field}")
    if result < 0 or result > 0xFFFFFFFF:
        raise GhidraToolError(f"invalid {field}")
    return result


def _validated_provenance(record: dict, *, reviewed_required: bool) -> None:
    provenance = record.get("provenance")
    if not isinstance(provenance, dict):
        raise GhidraToolError("recognized descriptor missing provenance")
    strings = [provenance.get(name) for name in ("tool", "tool_version", "timestamp")]
    reviewed = provenance.get("human_reviewed")
    if any(not isinstance(value, str) for value in strings) or not isinstance(reviewed, bool):
        raise GhidraToolError("recognized descriptor has malformed provenance")
    if reviewed_required and (not all(strings) or not reviewed):
        raise GhidraToolError("code-pointer descriptor requires reviewed provenance")


def _normalized_structured_record(record: dict, expected_sha: str) -> dict:
    """Validate and canonicalize recognized records exactly at consumer fields."""

    kind = record.get("kind")
    if kind not in ("logical_table_descriptor", "code_pointer_table_descriptor",
                     "address_table_candidate"):
        return record
    if record.get("rom_sha256") != expected_sha:
        raise GhidraToolError("structured hint ROM hash mismatch")
    normalized = dict(record)
    for field in ("base_address", "entry_width_bytes", "stride_bytes", "entry_count"):
        normalized[field] = _consumer_uint32(record.get(field), field)
    if normalized["stride_bytes"] == 0 or normalized["entry_count"] == 0:
        raise GhidraToolError("recognized descriptor has zero stride or count")
    if kind == "logical_table_descriptor":
        _validated_provenance(record, reviewed_required=False)
        return normalized
    if kind == "address_table_candidate":
        # SEG-007-T204 / ADR-0033: raw, explicitly non-authoritative Ghidra
        # "Create Address Tables" analyzer output. Same shape constraints as
        # `code_pointer_table_descriptor` (4-byte absolute code pointers,
        # bounded span/count), but it is never itself trusted here or by the
        # C++ consumer -- it requires no `source`/`pointer_type` fields
        # (those are ADR-0032's human-reviewed-descriptor vocabulary; this
        # kind carries no semantic assertion beyond "Ghidra's analyzer
        # inferred a pointer array here") and never requires reviewed
        # provenance.
        if normalized["entry_width_bytes"] != 4 or normalized["stride_bytes"] != 4:
            raise GhidraToolError("malformed address-table candidate width/stride")
        if (normalized["entry_count"] > CODE_POINTER_TABLE_MAX_ENTRIES or
                normalized["base_address"] + normalized["entry_count"] * 4 > 0x100000000):
            raise GhidraToolError("address-table candidate span exceeds address space")
        _validated_provenance(record, reviewed_required=False)
        return normalized
    if (record.get("source") != "immutable_cartridge" or
            record.get("pointer_type") != "absolute_code_address" or
            normalized["entry_width_bytes"] != 4 or normalized["stride_bytes"] != 4):
        raise GhidraToolError("malformed code-pointer descriptor semantics")
    if (normalized["entry_count"] > CODE_POINTER_TABLE_MAX_ENTRIES or
            normalized["base_address"] + normalized["entry_count"] * 4 > 0x100000000):
        raise GhidraToolError("code-pointer descriptor span exceeds address space")
    _validated_provenance(record, reviewed_required=True)
    return normalized


def _structured_identity(record: dict) -> tuple:
    """Identity key for detecting genuinely-conflicting duplicates of the same
    recognised structured record. ``logical_table_descriptor`` is keyed by its
    ``base_address`` (ADR-0023's table identity); any other / future kind is
    keyed by its whole canonical body, so byte-identical copies dedup but two
    different bodies are never silently merged."""

    kind = record.get("kind")
    if kind == "logical_table_descriptor":
        return (kind, record.get("base_address"), record.get("entry_width_bytes"))
    if kind == "code_pointer_table_descriptor":
        return (kind, record.get("base_address"))
    if kind == "address_table_candidate":
        return (kind, record.get("base_address"), record.get("entry_width_bytes"))
    return (kind, _canonical_json(record))


def _structured_semantics(record: dict) -> str:
    """Canonical semantic body used for same-identity conflict checks.

    Provenance is required metadata at the consuming parser boundary, but is
    not descriptor semantics (ADR-0023/ADR-0032). Thus independently reviewed
    copies may coexist; differing count/shape fields still conflict.
    """

    semantic = dict(record)
    semantic.pop("provenance", None)
    return _canonical_json(semantic)


def compose_hints(
    candidates_path: Path,
    base_hints_path: Path | None,
    rom_path: Path | None,
    rom_sha256_arg: str | None,
    output_path: Path,
    address_table_candidates_path: Path | None = None,
    compat_genesis_path: Path | None = None,
) -> None:
    """ADR-0025: build the canonical heterogeneous one-shot analysis artifact as
    the UNION of the pre-existing ROM-bound structured hints for this ROM and
    the freshly generated Ghidra ``code_entry_candidate`` proposals.

    Offline Ghidra analysis AUGMENTS, never REPLACES, existing structured hints.
    Every recognised heterogeneous record kind is preserved semantically;
    descriptor scalar spellings are normalized through their consumer grammar
    before comparison and output. ``code_entry_candidate`` records from
    both inputs are de-duplicated by address. The composition verifies a single
    matching ``rom_sha256`` across all inputs, fails closed on incompatible ROM
    identity or genuinely conflicting structured records, and never overwrites
    the canonical private structured-hints source file.

    SEG-007-T204 / ADR-0033: when ``address_table_candidates_path`` is given,
    its ``address_table_candidate`` records (raw, explicitly non-authoritative
    Ghidra "Create Address Tables" analyzer output; see
    ``ExportAddressTableCandidates.java``) are unioned in exactly like any
    other recognised structured record kind -- preserved, never trusted, never
    reinterpreted as a ``logical_table_descriptor``.

    SEG-007-T206 / ADR-0034: ``compat_genesis_path`` (default
    ``platforms/genesis/compat/<rom-sha256>.json`` under THIS invocation's own current
    worktree/``PROJECT_ROOT`` -- not the shared main worktree root) is unioned
    in exactly like ``base_hints_path`` -- same ROM-identity precondition,
    same conflict-detection rule for a disagreeing duplicate identity, same
    normalization. Unlike ``base_hints_path``, an absent or empty compat file
    is a complete no-op (never a hard error): this is the only durable home
    for a SEG-007-T206-style bounded, development-time table-extent
    assertion, and ordinary recompilation must not fail merely because a
    given ROM has none committed yet.

    ``platforms/genesis/compat/`` is a git-tracked, per-branch artifact (unlike the
    ignored, intentionally cross-worktree-shared ``.tools/analysis-hints/``
    cache resolved via ``_main_worktree_root()``): a task's own committed
    compat file lives only in that task's own worktree/branch checkout until
    it merges. Resolving its default path via the shared main worktree root
    would silently miss a not-yet-merged task's own committed compat file
    when ``compose-hints`` is run from that task's own worktree -- this
    project's standard delivery pattern -- unless ``--compat-genesis`` is
    passed explicitly. The default therefore resolves against
    ``PROJECT_ROOT`` (this invocation's own current worktree), matching how
    every other git-tracked, per-branch source file in this repository is
    read.
    """

    if rom_path is not None:
        if not rom_path.is_file():
            raise GhidraToolError(f"ROM not found: {rom_path}")
        expected_sha = hashlib.sha256(rom_path.read_bytes()).hexdigest()
        if rom_sha256_arg and rom_sha256_arg != expected_sha:
            raise GhidraToolError("--rom-sha256 does not match --rom contents")
    elif rom_sha256_arg:
        expected_sha = rom_sha256_arg
    else:
        raise GhidraToolError("compose-hints requires --rom or --rom-sha256")

    if base_hints_path is None:
        base_hints_path = (
            _main_worktree_root() / ".tools" / "analysis-hints" / f"{expected_sha}.json"
        )
    base_hints_path = base_hints_path.resolve()
    if compat_genesis_path is None:
        compat_genesis_path = PROJECT_ROOT / "platforms" / "genesis" / "compat" / f"{expected_sha}.json"
    compat_genesis_path = compat_genesis_path.resolve()
    output_path = output_path.resolve()
    candidates_path = candidates_path.resolve()
    if output_path == base_hints_path or output_path == compat_genesis_path or (
        output_path.parent.name == "analysis-hints"
        and output_path.name == f"{expected_sha}.json"
    ) or (
        output_path.parent.name == "compat" and output_path.parent.parent.name == "genesis"
        and output_path.name == f"{expected_sha}.json"
    ):
        raise GhidraToolError(
            "refusing to write the composed artifact over a canonical "
            "per-ROM structured-hints source file"
        )
    if not candidates_path.is_file():
        raise GhidraToolError(f"candidate export not found: {candidates_path}")
    if not base_hints_path.is_file():
        raise GhidraToolError(f"pre-existing structured hints not found: {base_hints_path}")

    candidate_payload = _strict_json_load(candidates_path)
    if not isinstance(candidate_payload, list):
        raise GhidraToolError("candidate export is not a JSON array")
    base_payload = _strict_json_load(base_hints_path)
    if not isinstance(base_payload, list):
        raise GhidraToolError("structured hints file is not a JSON array")
    if address_table_candidates_path is not None:
        address_table_candidates_path = address_table_candidates_path.resolve()
        if not address_table_candidates_path.is_file():
            raise GhidraToolError(
                f"address-table candidate export not found: {address_table_candidates_path}")
        address_table_payload = _strict_json_load(address_table_candidates_path)
        if not isinstance(address_table_payload, list):
            raise GhidraToolError("address-table candidate export is not a JSON array")
        for entry in address_table_payload:
            if not isinstance(entry, dict) or entry.get("kind") != "address_table_candidate":
                raise GhidraToolError("expected an address_table_candidate record")
        base_payload = base_payload + address_table_payload

    # SEG-007-T206 / ADR-0034: additive-only union of the committed, narrow
    # platforms/genesis/compat/<rom-sha256>.json scalar assertion file. Absent or
    # empty is a complete no-op; a present-but-malformed file (not a JSON
    # array, or a non-object entry) fails closed exactly like any other
    # malformed structured-hints input.
    if compat_genesis_path.is_file():
        compat_payload = _strict_json_load(compat_genesis_path)
        if not isinstance(compat_payload, list):
            raise GhidraToolError("platforms/genesis/compat file is not a JSON array")
        for entry in compat_payload:
            if not isinstance(entry, dict):
                raise GhidraToolError("platforms/genesis/compat file has a non-object record")
        base_payload = base_payload + compat_payload

    by_address: dict[int, dict] = {}
    for entry in candidate_payload:
        record = _validated_candidate_record(entry, expected_sha)
        by_address.setdefault(record["address"], record)

    structured_semantics: dict[tuple, str] = {}
    structured_by_body: dict[str, dict] = {}
    for entry in base_payload:
        if not isinstance(entry, dict):
            raise GhidraToolError("structured hints file has a non-object record")
        if entry.get("kind") == CANONICAL_CANDIDATE_KIND:
            record = _validated_candidate_record(entry, expected_sha)
            by_address.setdefault(record["address"], record)
            continue
        if entry.get("rom_sha256") != expected_sha:
            raise GhidraToolError("structured hint ROM hash mismatch")
        record = _normalized_structured_record(entry, expected_sha)
        identity = _structured_identity(record)
        semantics = _structured_semantics(record)
        existing = structured_semantics.get(identity)
        if existing is not None and existing != semantics:
            raise GhidraToolError(
                f"conflicting structured records for kind {identity[0]!r}"
            )
        structured_semantics[identity] = semantics
        # Exact copies deduplicate; provenance variants remain distinct and
        # are ordered below by their complete canonical bodies.
        body = _canonical_json(record)
        structured_by_body[body] = record

    composed: list[dict] = sorted(
        structured_by_body.values(),
        key=_canonical_json,
    )
    composed.extend(sorted(by_address.values(), key=lambda r: r["address"]))

    canonical = _canonical_artifact(composed)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(canonical, encoding="utf-8", newline="\n")

    kinds: dict[str, int] = {}
    for record in composed:
        key = str(record.get("kind"))
        kinds[key] = kinds.get(key, 0) + 1
    summary = " ".join(f"{name}={kinds[name]}" for name in sorted(kinds))
    print(f"compose-hints: rom_sha256={expected_sha} {summary} output={output_path}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--runtime-root",
        type=Path,
        default=DEFAULT_RUNTIME_ROOT,
        help="ignored directory for token and read-only analysis inputs",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("init", help="create the secret and input directory")
    subparsers.add_parser("config", help="validate the resolved Compose configuration")
    subparsers.add_parser("build", help="build pinned Ghidra and bridge images")
    subparsers.add_parser("up", help="build and start both services, waiting for health")
    subparsers.add_parser("down", help="stop services while preserving project volumes")
    subparsers.add_parser("status", help="show service state")
    subparsers.add_parser("health", help="fail unless both services are running")
    subparsers.add_parser("stdio", help="run the MCP bridge over stdio for an MCP client")
    logs_parser = subparsers.add_parser("logs", help="follow service logs")
    logs_parser.add_argument("service", nargs="?", choices=("ghidra", "bridge"))
    analyze_parser = subparsers.add_parser(
        "analyze", help="deterministic offline whole-ROM code-entry candidate analysis (ADR-0025)"
    )
    analyze_parser.add_argument("--rom", type=Path, required=True)
    analyze_parser.add_argument("--output", type=Path, required=True)
    analyze_parser.add_argument(
        "--address-table-output", type=Path,
        help="SEG-007-T204 / ADR-0033: output path for the sibling, explicitly "
        "non-authoritative address_table_candidate export; defaults to "
        "'<output stem>.address-tables.json' beside --output",
    )
    compose_parser = subparsers.add_parser(
        "compose-hints",
        help="union pre-existing ROM-bound structured hints with generated Ghidra "
        "code_entry_candidate records into the canonical one-shot artifact (ADR-0025)",
    )
    compose_parser.add_argument(
        "--candidates", type=Path, required=True,
        help="Ghidra code_entry_candidate export from 'ghidra.py analyze'",
    )
    compose_parser.add_argument(
        "--address-table-candidates", type=Path,
        help="SEG-007-T204 / ADR-0033: optional Ghidra address_table_candidate "
        "export from 'ghidra.py analyze --address-table-output'",
    )
    compose_parser.add_argument(
        "--base-hints", type=Path,
        help="pre-existing structured hints file; defaults to the canonical "
        "per-ROM path resolved from the ROM hash",
    )
    compose_parser.add_argument(
        "--compat-genesis", type=Path,
        help="SEG-007-T206 / ADR-0034: optional committed platforms/genesis/compat/<sha>.json "
        "scalar assertion file; defaults to the canonical per-ROM path resolved "
        "from the ROM hash; absent/empty is a complete no-op",
    )
    compose_parser.add_argument("--rom", type=Path)
    compose_parser.add_argument("--rom-sha256")
    compose_parser.add_argument("--output", type=Path, required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    runtime_root = args.runtime_root.resolve()
    try:
        env_file = ensure_runtime(runtime_root)
        if args.command == "init":
            print(f"Runtime initialized: {runtime_root}")
            print(f"Secret: {env_file} (0600)")
            print(f"Place legal analysis inputs in: {runtime_root / 'input'}")
        elif args.command == "config":
            run(runtime_root, "config", "--quiet")
            print("Compose configuration is valid")
        elif args.command == "build":
            run(runtime_root, "build")
        elif args.command == "up":
            run(runtime_root, "up", "--build", "--detach", "--wait")
            print("Ghidra and MCP bridge are healthy")
        elif args.command == "down":
            run(runtime_root, "down", "--remove-orphans")
        elif args.command == "status":
            run(runtime_root, "ps")
        elif args.command == "health":
            services = running_services(runtime_root)
            missing = sorted({"ghidra", "bridge"} - services)
            if missing:
                raise GhidraToolError(f"unhealthy or stopped services: {', '.join(missing)}")
            print("OK: ghidra and bridge are running")
        elif args.command == "logs":
            arguments = ["logs", "--follow", "--tail", "200"]
            if args.service:
                arguments.append(args.service)
            run(runtime_root, *arguments)
        elif args.command == "stdio":
            exec_stdio(runtime_root)
        elif args.command == "analyze":
            analyze(
                runtime_root, args.rom.resolve(), args.output.resolve(),
                args.address_table_output.resolve() if args.address_table_output else None,
            )
        elif args.command == "compose-hints":
            compose_hints(
                args.candidates,
                args.base_hints if args.base_hints else None,
                args.rom.resolve() if args.rom else None,
                args.rom_sha256,
                args.output,
                args.address_table_candidates if args.address_table_candidates else None,
                args.compat_genesis if args.compat_genesis else None,
            )
    except (GhidraToolError, OSError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"ghidra: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
