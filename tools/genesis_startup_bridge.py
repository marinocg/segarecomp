#!/usr/bin/env python3
"""Generate, compile, and run the bounded general-startup bridge.

This tool intentionally accepts only a generation-time ROM input.  It never
prints a full runtime state for commercial mode and never implements CPU or
device behavior itself.
"""
import argparse
import base64
import contextlib
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import threading

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from tools import process_tree  # noqa: E402


# These are the pinned T028 wire names, not host enum ordinals.  Keep the
# coarse stop class and its permitted diagnostic names together so malformed
# generated reports cannot be mistaken for another frontier.
STOP_DIAGNOSTIC_PAIRS = {
    "unsupported_cpu_form": {
        "valid_but_unsupported_instruction", "unsupported_instruction_form",
    },
    "unsupported_device_access": {
        "unsupported_device_region_controller_io", "unsupported_device_region_vdp",
        "unsupported_device_region_z80_bus", "unsupported_device_region_z80_ram",
        "unsupported_device_region_psg",
    },
    "unsupported_memory_region": {
        "effective_address_not_24bit", "odd_effective_address", "rom_write_prohibited",
        "unmapped_data_access", "invalid_stack_alignment", "invalid_stack_range",
        "return_target_mismatch",
    },
    # SEG-007-T174 / ADR-0024: Tier 2's own diagnostic shares this stop class
    # with Tier 1's existing "reached_unresolved_direct_edge".
    "unresolved_indirect_target": {
        "reached_unresolved_direct_edge",
        "tier2_computed_target_not_emitted",
    },
    "known_but_unemitted_target": {"known_but_unemitted_target"},
    "unsupported_interrupt_or_scheduling_event": set(),
    "internal_dispatch_inconsistency": {"internal_dispatch_inconsistency"},
    # SEG-007-T252 / ADR-0040: this stop pair is now unreachable via the
    # runner-owned genesis_runtime_run path -- there is no longer any
    # generated-runtime progress watchdog left to produce it. It is retained
    # here only for wire/ABI stability (a foreign or historical producer of
    # this exact enum pair must still validate); a genuine runner-side
    # exhaustion instead reports the disjoint top-level "runner_resource_limit"
    # result (see valid_sanitized/valid_full), never this stop_class/
    # diagnostic_category pair.
    "instruction_budget_exhausted": {"instruction_budget_exhausted"},
    # ADR 0013 Decision §6: the static-discovery-stage prefix boundary lowers
    # its real diagnostic category through GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED.
    "discovery_prefix_boundary": {"discovery_budget_exhausted"},
    "c4_lowering_gap": {"c4_lowering_gap"},
    # SEG-021-T018 / ADR 0043: a CPU exception delivered fail-closed at run time.
    "unsupported_cpu_exception": {
        "unsupported_privilege_violation_exception", "unsupported_trace_exception",
    },
}

# One whitelist entry per finite, stop-owned C4 family emitted by
# `c4_lowering_dimension_literal` and named by
# `genesis_c4_lowering_dimensions_fields`.  Keep this list exhaustive: the
# bridge validates a report pair, not a generic C4 family string.  In
# particular, the compare and subtract dimensions below are existing ADR-0015
# values, not permissive fallbacks for future lowering shapes.
# Deliberately no generic "other" catch-all: a shape not in this whitelist is
# invalid evidence, matching the emitter's own fail-closed rejection for an
# unrepresented shape.
C4_LOWERING_DIMENSIONS = (
    {"family": "branch_ne_short_missing_dispatcher"},
    {"family": "branch_always_short_missing_dispatcher"},
    {"family": "compare_missing_dispatcher"},
    {"family": "compare_immediate_missing_dispatcher"},
    {"family": "add_immediate_missing_dispatcher"},
    {"family": "add_quick_missing_dispatcher"},
    {"family": "subtract_missing_dispatcher"},
    {"family": "subtract_immediate_missing_dispatcher"},
    {"family": "logical_and_missing_dispatcher"},
    {"family": "logical_or_missing_dispatcher"},
    {"family": "logical_or_immediate_missing_dispatcher"},
    {"family": "exclusive_or_missing_dispatcher"},
    {"family": "exclusive_or_immediate_missing_dispatcher"},
    {"family": "write_swap_missing_dispatcher"},
    {"family": "sign_extend_word_missing_dispatcher"},
    {"family": "sign_extend_long_missing_dispatcher"},
    {"family": "push_effective_address_missing_dispatcher"},
    {"family": "link_frame_missing_dispatcher"},
    {"family": "unlink_frame_missing_dispatcher"},
    {"family": "bit_change_missing_dispatcher"},
    {"family": "bit_clear_missing_dispatcher"},
    {"family": "bit_set_missing_dispatcher"},
    {"family": "shift_rotate_register_missing_dispatcher"},
    {"family": "shift_rotate_memory_missing_dispatcher"},
    # There is deliberately no "movem_missing_routing" entry: that shape is
    # advisory-only in the preflight inventory and never reaches an emitted
    # GENESIS_STOP_C4_LOWERING_GAP stop (see platforms/genesis/runtime/runtime.h).
    {"family": "add_auto_update"},
    {"family": "clr_auto_update"},
    {"family": "movea_auto_update"},
    {"family": "adda_auto_update"},
    {"family": "suba_auto_update"},
    {"family": "cmpa_auto_update"},
    {"family": "cmp_auto_update"},
    {"family": "cmpi_auto_update"},
    {"family": "bit_test_auto_update"},
    {"family": "tst_auto_update"},
    {"family": "andi_auto_update"},
    # SEG-007-T167: logical family (AND/OR/EOR + ORI/EORI) auto-update declines.
    {"family": "logical_and_auto_update"},
    {"family": "logical_or_auto_update"},
    {"family": "logical_or_immediate_auto_update"},
    {"family": "exclusive_or_auto_update"},
    {"family": "exclusive_or_immediate_auto_update"},
    # SEG-007-T168: NOT (logical complement) shares the sibling logical
    # family's declined-auto-update shape.
    {"family": "logical_not_auto_update"},
    {"family": "move_missing_fact"},
    {"family": "movea_missing_fact"},
    {"family": "add_missing_fact"},
    {"family": "adda_missing_fact"},
    {"family": "suba_missing_fact"},
    {"family": "cmpa_missing_fact"},
    {"family": "cmp_missing_fact"},
    {"family": "cmpi_missing_fact"},
    {"family": "bit_test_missing_fact"},
    {"family": "tst_missing_fact"},
    {"family": "clr_missing_fact"},
    {"family": "andi_missing_fact"},
    {"family": "add_quick_missing_fact"},
    # SEG-007-T167: represented logical-family foldable memory operand, no fact.
    {"family": "logical_and_missing_fact"},
    {"family": "logical_or_missing_fact"},
    {"family": "logical_or_immediate_missing_fact"},
    {"family": "exclusive_or_missing_fact"},
    {"family": "exclusive_or_immediate_missing_fact"},
    # SEG-007-T168: represented NOT with a statically foldable memory
    # destination and no retained resolver fact.
    {"family": "logical_not_missing_fact"},
    # The subtract family uses the same existing stop-owned C4 schema as the
    # earlier families; its literals were present in the generated runtime
    # but absent from this bridge-side report-pair whitelist.
    {"family": "subtract_auto_update"},
    {"family": "subtract_immediate_auto_update"},
    {"family": "subtract_missing_fact"},
    {"family": "subtract_immediate_missing_fact"},
    # SEG-007-T209: BCHG/BCLR/BSET share BTST's declined-auto-update /
    # missing-fact schema shape, added to the generated runtime alongside
    # this task's own new emission dispatcher.
    {"family": "bit_change_auto_update"},
    {"family": "bit_clear_auto_update"},
    {"family": "bit_set_auto_update"},
    {"family": "bit_change_missing_fact"},
    {"family": "bit_clear_missing_fact"},
    {"family": "bit_set_missing_fact"},
    # SEG-007-T220: MULS.W shares CMP's declined-auto-update / missing-fact
    # schema shape (read-only source, fixed Dn destination).
    {"family": "muls_auto_update"},
    {"family": "muls_missing_fact"},
)


def report_sha_status(report: object, digest: str) -> int:
    """Return the T028 driver status for a report's SHA field.

    A well-formed present SHA is an identity check. Missing, non-string, and
    malformed values are wire-schema errors rather than hash mismatches.
    """
    if (not isinstance(report, dict) or "rom_sha256" not in report or
            not isinstance(report["rom_sha256"], str) or len(report["rom_sha256"]) != 64 or
            any(character not in "0123456789abcdef" for character in report["rom_sha256"])):
        return 5
    return 0 if report["rom_sha256"] == digest else 4


def hex_address(value: str) -> str:
    try:
        number = int(value, 16)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be hexadecimal") from error
    if number < 0 or number > 0xFFFFFF or number & 1:
        raise argparse.ArgumentTypeError("must be an even 24-bit address")
    return f"{number:08x}"


def hex_mapping_base(value: str) -> str:
    try:
        number = int(value, 16)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be hexadecimal") from error
    if number < 0 or number > 0xFFFFFF:
        raise argparse.ArgumentTypeError("must be a 24-bit address")
    return f"{number:08x}"


UINT32_MAX = (1 << 32) - 1

# SEG-007-T252 / ADR-0040 correction: the named canonical/headless runner
# dispatch allowance. This is HOST RUNNER POLICY ONLY -- it is not hardware
# timing, not guest CPU/device semantics, not a discovery-completeness knob,
# and not an instruction-count correctness claim. The former semantic-
# watchdog window (historically pinned at 128) was a *consecutive no-progress*
# window that legitimate recurring guest progress kept resetting, so real
# execution ran for millions of dispatches; the generated bridge's own
# zero-argument compiled-in default of 128 is a *total* dispatch count and is
# NOT a behaviorally equivalent replacement for that watchdog window. This
# constant is the canonical/headless CLI's own default total dispatch
# allowance for its one-shot `--diagnose-frontier` route when the operator
# does not explicitly pass `--instruction-budget`; it is always explicitly
# overridable via `--instruction-budget`.
GENESIS_CANONICAL_RUNNER_DISPATCH_ALLOWANCE = 16777216


def instruction_budget_value(value: str) -> int:
    """SEG-007-T252 / ADR-0040: the runner-owned finite dispatch allowance.

    This tool is itself the automated/headless CLI entry point, so it always
    rejects an explicit 0 request here (even though the wire schema's
    `GenesisDeterministicOptions.instruction_budget` field still tolerates a
    foreign-produced 0 meaning "no bound named" -- this validator is not that
    schema check). Malformed (non-digit), negative, fractional, and
    out-of-range (> UINT32_MAX) values are all rejected; UINT32_MAX itself is
    accepted.
    """
    if not value or not all(character in "0123456789" for character in value):
        raise argparse.ArgumentTypeError("must be a non-negative base-10 integer")
    number = int(value)
    if number == 0:
        raise argparse.ArgumentTypeError("must be nonzero for this automated/headless entry point")
    if number > UINT32_MAX:
        raise argparse.ArgumentTypeError("must not exceed UINT32_MAX")
    return number


def ignored(path: pathlib.Path, root: pathlib.Path) -> bool:
    return subprocess.run(["git", "check-ignore", "-q", "--", str(path)], cwd=root,
                          capture_output=True, text=True).returncode == 0


def _writer_token(write_fd: int) -> str:
    """The generated program's writer token for one parent-owned pipe write end.

    POSIX: the inherited descriptor number. Windows (SEG-018-T006): the native Win32
    HANDLE value; the child converts it to its own CRT fd (a parent CRT fd number is
    not a valid identity there). The historical `-fd` argv spelling carries the token.
    """
    if os.name != "nt":
        return str(write_fd)
    import msvcrt
    return str(msvcrt.get_osfhandle(write_fd))


@contextlib.contextmanager
def _inherit_writers(write_fds: tuple):
    """Yield Popen kwargs that make exactly these pipe write ends inheritable.

    POSIX: ``pass_fds``. Windows: only the listed native handles are marked inheritable
    (temporarily) and named in an explicit ``handle_list`` with ``close_fds=True``; no
    other handle leaks to the child and the parent's flags are restored afterwards.
    """
    if os.name != "nt":
        yield {"pass_fds": tuple(write_fds)}
        return
    import msvcrt
    handles = [msvcrt.get_osfhandle(fd) for fd in write_fds]
    for handle in handles:
        os.set_handle_inheritable(handle, True)
    try:
        if not handles:
            yield {}
            return
        startup = subprocess.STARTUPINFO()
        startup.lpAttributeList = {"handle_list": handles}
        yield {"startupinfo": startup, "close_fds": True}
    finally:
        for handle in handles:
            try:
                os.set_handle_inheritable(handle, False)
            except OSError:
                pass


def run_bridge(executable: pathlib.Path, root: pathlib.Path, full_path: pathlib.Path | None = None,
                full_pipe: bool = False, instruction_budget: int | None = None,
                ephemeral_pipe: bool = False) -> tuple[int, dict, bytes | None, str, bytes | None]:
    command = process_tree.script_argv(executable)
    # SEG-007-T252 / ADR-0040: an explicit finite runner dispatch allowance.
    # When omitted, the generated binary uses its own compiled-in default
    # (128) -- this tool never silently changes that default on its own.
    if instruction_budget is not None:
        command += ["--instruction-budget", str(instruction_budget)]
    if full_path is not None:
        command += ["--full-report-path", str(full_path)]
    ephemeral_read_fd = -1
    ephemeral_write_fd = -1
    if ephemeral_pipe:
        # SEG-007-T252 / ADR-0040 correction: a SECOND, fully independent pipe
        # for the ephemeral-only PC-history channel -- distinct pass_fds and a
        # distinct `--ephemeral-report-fd` argv flag from the full-report pipe
        # below. Never a filesystem-path form.
        ephemeral_read_fd, ephemeral_write_fd = os.pipe()
        command += ["--ephemeral-report-fd", _writer_token(ephemeral_write_fd)]
    if not full_pipe:
        pass_fds = (ephemeral_write_fd,) if ephemeral_pipe else ()
        try:
            with _inherit_writers(pass_fds) as inherit_kwargs:
                completed = subprocess.run(command, text=True, capture_output=True, cwd=root, **inherit_kwargs)
        finally:
            if ephemeral_pipe:
                os.close(ephemeral_write_fd)
        full_bytes = None
        if ephemeral_pipe:
            chunks = []
            while True:
                chunk = os.read(ephemeral_read_fd, 65536)
                if not chunk:
                    break
                chunks.append(chunk)
            os.close(ephemeral_read_fd)
            ephemeral_bytes = b"".join(chunks)
        else:
            ephemeral_bytes = None
    else:
        read_fd, write_fd = os.pipe()
        command += ["--full-report-fd", _writer_token(write_fd)]
        pass_fds = (write_fd, ephemeral_write_fd) if ephemeral_pipe else (write_fd,)
        try:
            with _inherit_writers(pass_fds) as inherit_kwargs:
                child = subprocess.Popen(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         cwd=root, **inherit_kwargs)
            os.close(write_fd)
            write_fd = -1
            if ephemeral_pipe:
                os.close(ephemeral_write_fd)
                ephemeral_write_fd = -1
            chunks = []
            while True:
                chunk = os.read(read_fd, 65536)
                if not chunk:
                    break
                chunks.append(chunk)
            ephemeral_chunks = []
            if ephemeral_pipe:
                while True:
                    chunk = os.read(ephemeral_read_fd, 65536)
                    if not chunk:
                        break
                    ephemeral_chunks.append(chunk)
            completed_stdout, completed_stderr = child.communicate()
            completed = subprocess.CompletedProcess(command, child.returncode, completed_stdout, completed_stderr)
            full_bytes = b"".join(chunks)
            ephemeral_bytes = b"".join(ephemeral_chunks) if ephemeral_pipe else None
        finally:
            if write_fd >= 0:
                os.close(write_fd)
            os.close(read_fd)
            if ephemeral_pipe:
                if ephemeral_write_fd >= 0:
                    os.close(ephemeral_write_fd)
                os.close(ephemeral_read_fd)
    if completed.returncode != 0:
        return 3, {}, None, completed.stderr or "generated program exited nonzero\n", ephemeral_bytes
    # A generated program owns exactly one newline-terminated stdout record.
    # Any malformed, zero, or multiple line output is an execution failure.
    if not completed.stdout.endswith("\n") or completed.stdout.count("\n") != 1:
        return 3, {}, None, "generated program did not write exactly one line\n", ephemeral_bytes
    lines = completed.stdout.splitlines()
    try:
        report = json.loads(lines[0])
    except json.JSONDecodeError:
        return 3, {}, None, "generated program wrote invalid JSON\n", ephemeral_bytes
    if json.dumps(report, separators=(",", ":"), ensure_ascii=False) != lines[0]:
        return 5, {}, None, "generated program wrote noncanonical JSON\n", ephemeral_bytes
    return 0, report, full_bytes, "", ephemeral_bytes


def parse_ephemeral_pc_history(data: bytes) -> list[str] | None:
    """Parse the ephemeral-report-fd channel's bare JSON PC-history array.

    SEG-007-T252 / ADR-0040 correction: this channel is diagnostic-only and
    must never be able to fail an otherwise-successful run, so any parse or
    format failure returns None ("no history available") rather than a hard
    driver error.
    """
    if not data:
        return None
    try:
        text = data.decode()
    except UnicodeDecodeError:
        return None
    # SEG-020-T003: line 1 is the PC array; an optional line 2 is the typed
    # execution history (parsed separately). Any other shape is "unavailable".
    if not text.endswith("\n") or text.count("\n") not in (1, 2):
        return None
    line = text.splitlines()[0]
    try:
        history = json.loads(line)
    except json.JSONDecodeError:
        return None
    if json.dumps(history, separators=(",", ":"), ensure_ascii=False) != line:
        return None
    if not isinstance(history, list) or any(not hexadecimal(value, 8) for value in history):
        return None
    return history


def parse_ephemeral_execution_history(data: bytes) -> list[dict] | None:
    """SEG-020-T003: parse the optional second line of the ephemeral channel.

    Present only for binaries generated with --provenance-diagnostics. Any
    absence or malformation returns None and can never fail a run.
    """
    if not data:
        return None
    try:
        lines = data.decode().split("\n")
    except UnicodeDecodeError:
        return None
    if len(lines) != 3 or lines[2] != "":
        return None
    try:
        history = json.loads(lines[1])
    except json.JSONDecodeError:
        return None
    if json.dumps(history, separators=(",", ":"), ensure_ascii=False) != lines[1]:
        return None
    if not isinstance(history, list) or any(not isinstance(event, dict) or "b" not in event or "k" not in event
                                            for event in history):
        return None
    return history


def with_execution_history(frontier: dict, ephemeral_bytes: bytes | None) -> dict:
    history = parse_ephemeral_execution_history(ephemeral_bytes if ephemeral_bytes is not None else b"")
    if history:
        frontier["execution_history"] = history
    return frontier


def valid_sanitized(report: dict, digest: str) -> bool:
    required = {"schema_version", "report_kind", "rom_sha256", "result", "stop_class", "diagnostic_category", "cpu_dimensions", "c4_lowering_dimensions"}
    ordered = ["schema_version", "report_kind", "rom_sha256", "result", "stop_class", "diagnostic_category", "cpu_dimensions", "c4_lowering_dimensions"]
    # SEG-007-T252 / ADR-0040: runner_resource_limit is a sibling top-level
    # result carrying one additional deterministic field, `runner_dispatch_
    # count` -- checked here as its own disjoint shape, never folded into the
    # STOP_DIAGNOSTIC_PAIRS-keyed `stop_class`/`diagnostic_category` schema
    # below, and never confusable with the guest `instruction_budget_
    # exhausted` stop (which always reports `"result":"stop"`).
    if isinstance(report, dict) and report.get("result") == "runner_resource_limit":
        runner_ordered = ordered + ["runner_dispatch_count"]
        runner_required = required | {"runner_dispatch_count"}
        return (list(report) == runner_ordered and set(report) == runner_required and
                plain_int(report.get("schema_version")) and report.get("schema_version") == 1 and
                report.get("report_kind") == "sanitized" and report.get("rom_sha256") == digest and
                report.get("stop_class") is None and report.get("diagnostic_category") is None and
                report.get("cpu_dimensions") is None and report.get("c4_lowering_dimensions") is None and
                plain_int(report.get("runner_dispatch_count"), 1, (1 << 32) - 1))
    if (not isinstance(report, dict) or list(report) != ordered or set(report) != required or
            not plain_int(report.get("schema_version")) or report.get("schema_version") != 1 or
            report.get("report_kind") != "sanitized" or report.get("rom_sha256") != digest):
        return False
    if report.get("result") == "completed":
        return report.get("stop_class") is None and report.get("diagnostic_category") is None and report.get("cpu_dimensions") is None and report.get("c4_lowering_dimensions") is None
    if (report.get("result") != "stop" or not isinstance(report.get("stop_class"), str) or
            not isinstance(report.get("diagnostic_category"), str) or
            report["stop_class"] not in STOP_DIAGNOSTIC_PAIRS or
            report["diagnostic_category"] not in STOP_DIAGNOSTIC_PAIRS[report["stop_class"]]):
        return False
    dimensions = report.get("cpu_dimensions")
    c4_dimensions = report.get("c4_lowering_dimensions")
    if report["stop_class"] == "c4_lowering_gap":
        return dimensions is None and isinstance(c4_dimensions, dict) and list(c4_dimensions) == ["family"] and c4_dimensions in C4_LOWERING_DIMENSIONS
    if c4_dimensions is not None:
        return False
    if report["stop_class"] != "unsupported_cpu_form":
        return dimensions is None
    return (isinstance(dimensions, dict) and
            list(dimensions) == ["family", "size", "addressing_mode_class"] and
            dimensions in (
                {"family": "nop", "size": "none", "addressing_mode_class": "implied"},
                {"family": "stop", "size": "word", "addressing_mode_class": "immediate"},
                # SEG-007-T059 defined GENESIS_CPU_DIMENSIONS_RESET and its
                # genesis_cpu_dimensions_fields wire triple
                # (platforms/genesis/runtime/runtime.c) as a recognized no-operand System
                # Control Group CPU-frontier classification. SEG-007-T114 makes
                # RESET (0x4E70) the synthetic-fixture "verified complete CPU
                # frontier" sentinel in place of NOP (0x4E71), now that NOP is a
                # selected general_startup capability. Whitelisting the triple
                # here only lets the driver validate an already-representable
                # wire shape; RESET itself remains unemitted/unsupported.
                {"family": "reset", "size": "none", "addressing_mode_class": "implied"},
                # SEG-007-T081: this exact triple was already fully defined by
                # genesis_cpu_dimensions_fields (GENESIS_CPU_DIMENSIONS_MOVE_AN_TO_USP,
                # tools/genesis_startup_bridge_runtime.c) before this task began; this
                # task's own VDP control-port selector is what first makes generated
                # execution reach it. Whitelisting it here only lets the driver validate
                # an already-representable wire shape; it adds no CPU-instruction-support
                # implementation of any kind (MOVE to/from USP remains unemitted/unsupported).
                {"family": "move_to_usp", "size": "long", "addressing_mode_class": "address_register_direct"},
            ))


def plain_int(value: object, minimum: int = 0, maximum: int = (1 << 64) - 1) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and minimum <= value <= maximum


def hexadecimal(value: object, digits: int) -> bool:
    return isinstance(value, str) and len(value) == digits + 2 and value.startswith("0x") and all(
        character in "0123456789abcdef" for character in value[2:])


def valid_runtime(runtime: object) -> bool:
    if (not isinstance(runtime, dict) or
            list(runtime) != ["d", "a", "usp", "sr", "pc", "work_ram_base64"]):
        return False
    return (isinstance(runtime["d"], list) and len(runtime["d"]) == 8 and
            isinstance(runtime["a"], list) and len(runtime["a"]) == 8 and
            all(hexadecimal(value, 8) for value in runtime["d"] + runtime["a"]) and
            hexadecimal(runtime["usp"], 8) and hexadecimal(runtime["sr"], 4) and hexadecimal(runtime["pc"], 8) and
             isinstance(runtime["work_ram_base64"], str) and len(runtime["work_ram_base64"]) == 87384 and
             _valid_ram_base64(runtime["work_ram_base64"]))


def _valid_ram_base64(value: str) -> bool:
    try:
        decoded = base64.b64decode(value, validate=True)
        return len(decoded) == 65536 and base64.b64encode(decoded).decode("ascii") == value
    except (ValueError, TypeError):
        return False


# The `2..12` / `0..12` instruction-length and raw-byte-count bounds below are
# a hand-maintained mirror of the generated-runtime provenance ABI
# (GENESIS_MAX_RAW_BYTES == SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES == 12);
# see docs/decisions/0008-route-provenance-raw-instruction-bytes-capacity.md.
def valid_provenance(value: object) -> bool:
    if not isinstance(value, dict) or list(value) != ["has_instruction_provenance", "instruction", "has_access", "access_address", "access_width", "access_direction", "mapping_claim_count", "mapping_claims", "bus_access_count", "bus_accesses"]:
        return False
    if not isinstance(value["has_instruction_provenance"], bool) or not isinstance(value["has_access"], bool): return False
    instruction = value["instruction"]
    if value["has_instruction_provenance"]:
        if (not isinstance(instruction, dict) or
                list(instruction) != ["cpu_variant", "source_address", "image_offset", "primary_bytes", "length"] or
                instruction["cpu_variant"] != "mc68000" or not hexadecimal(instruction["source_address"], 8) or
                int(instruction["source_address"], 16) > 0xFFFFFF or
                not plain_int(instruction["image_offset"]) or not plain_int(instruction["length"], 2, 12) or
                not hexadecimal("0x" + instruction["primary_bytes"] if isinstance(instruction["primary_bytes"], str) else None, 4)):
            return False
    elif instruction is not None: return False
    if not hexadecimal(value["access_address"], 8) or int(value["access_address"], 16) > 0xFFFFFF: return False
    if value["has_access"]:
        if value["access_width"] not in ("byte", "word", "long") or value["access_direction"] not in ("read", "write"): return False
    elif (value["access_address"] != "0x00000000" or value["access_width"] is not None or
          value["access_direction"] is not None): return False
    claims, accesses = value["mapping_claims"], value["bus_accesses"]
    if (not isinstance(claims, list) or not plain_int(value["mapping_claim_count"], 0, 4) or
            value["mapping_claim_count"] != len(claims) or not isinstance(accesses, list) or
            not plain_int(value["bus_access_count"], 0, 4) or value["bus_access_count"] != len(accesses)): return False
    for claim in claims:
        if (not isinstance(claim, dict) or list(claim) != ["name", "target_begin", "target_end", "image_begin", "image_end"] or
                not isinstance(claim["name"], str) or len(claim["name"].encode("utf-8")) > 64 or
                not hexadecimal(claim["target_begin"], 8) or not hexadecimal(claim["target_end"], 8) or
                int(claim["target_begin"], 16) > 0xFFFFFF or int(claim["target_end"], 16) > 0xFFFFFF or
                int(claim["target_begin"], 16) >= int(claim["target_end"], 16) or
                not plain_int(claim["image_begin"]) or not plain_int(claim["image_end"]) or
                claim["image_begin"] >= claim["image_end"] or
                claim["image_end"] - claim["image_begin"] != int(claim["target_end"], 16) - int(claim["target_begin"], 16)):
            return False
    for index, access in enumerate(accesses):
        if (not isinstance(access, dict) or list(access) != ["ordinal", "kind", "address", "raw_bytes", "raw_byte_count", "region"] or
                not plain_int(access["ordinal"]) or access["ordinal"] != index or
                access["kind"] not in ("instruction_read", "data_read", "data_write", "stack_read", "stack_write") or
                not hexadecimal(access["address"], 8) or int(access["address"], 16) > 0xFFFFFF or
                access["region"] not in ("raw_cartridge_rom", "synthetic_work_ram") or
                not plain_int(access["raw_byte_count"], 0, 12) or
                not isinstance(access["raw_bytes"], str) or
                not hexadecimal("0x" + access["raw_bytes"], access["raw_byte_count"] * 2) or
                len(access["raw_bytes"]) != access["raw_byte_count"] * 2):
            return False
    return True


def valid_full(full: object, sanitized: dict, digest: str) -> bool:
    base_keys = ["schema_version", "report_kind", "rom_sha256", "result", "runtime", "stop_class", "diagnostic_category", "c4_lowering_dimensions", "provenance"]
    # SEG-007-T252 / ADR-0040: runner_resource_limit's own disjoint full-report
    # shape -- one extra deterministic `runner_dispatch_count` field, every
    # guest-stop field null, checked independently of the STOP_DIAGNOSTIC_
    # PAIRS-keyed shape below.
    if isinstance(full, dict) and full.get("result") == "runner_resource_limit":
        # SEG-007-T252 / ADR-0040 correction: recent-PC history is NEVER part
        # of this stable full-report schema -- it is emitted only by the
        # dedicated ephemeral writer (see platforms/genesis/runtime/runtime.c's
        # genesis_write_ephemeral_pc_history and this tool's own
        # parse_ephemeral_pc_history/ephemeral_frontier) -- so this schema
        # validator has no knowledge of it at all.
        if list(full) != base_keys + ["runner_dispatch_count"]:
            return False
        if (not plain_int(full.get("schema_version")) or full.get("schema_version") != 1 or
                full.get("report_kind") != "full" or full.get("rom_sha256") != digest or
                not valid_runtime(full.get("runtime"))):
            return False
        full_valid = (full["stop_class"] is None and full["diagnostic_category"] is None and
                      full["c4_lowering_dimensions"] is None and full["provenance"] is None and
                      plain_int(full.get("runner_dispatch_count"), 1, (1 << 32) - 1))
        return (full_valid and valid_sanitized(sanitized, digest) and
                full["result"] == sanitized.get("result") and
                full.get("runner_dispatch_count") == sanitized.get("runner_dispatch_count"))
    if not isinstance(full, dict) or list(full) != base_keys:
        return False
    if (not plain_int(full.get("schema_version")) or full.get("schema_version") != 1 or full.get("report_kind") != "full" or
            full.get("rom_sha256") != digest or not valid_runtime(full.get("runtime"))):
        return False
    if full["result"] == "completed":
        full_valid = full["stop_class"] is None and full["diagnostic_category"] is None and full["provenance"] is None and full["c4_lowering_dimensions"] is None
    elif (full["result"] == "stop" and isinstance(full["stop_class"], str) and
          isinstance(full["diagnostic_category"], str) and
          full["stop_class"] in STOP_DIAGNOSTIC_PAIRS and
          full["diagnostic_category"] in STOP_DIAGNOSTIC_PAIRS[full["stop_class"]]):
        c4_dimensions = full["c4_lowering_dimensions"]
        if full["stop_class"] == "c4_lowering_gap":
            full_valid = c4_dimensions in C4_LOWERING_DIMENSIONS and valid_provenance(full["provenance"])
        else:
            full_valid = c4_dimensions is None and valid_provenance(full["provenance"])
    else:
        return False
    # Validate both records before comparing their public fields.  This helper
    # is also called directly by focused validation, so a malformed peer must
    # fail here rather than relying on callers to have validated it first.
    return (full_valid and valid_sanitized(sanitized, digest) and
            full["result"] == sanitized.get("result") and
            full["stop_class"] == sanitized.get("stop_class") and
            full["diagnostic_category"] == sanitized.get("diagnostic_category") and
            full["c4_lowering_dimensions"] == sanitized.get("c4_lowering_dimensions"))


def parse_canonical_full(raw: bytes | str) -> dict | None:
    try:
        text = raw.decode() if isinstance(raw, bytes) else raw
        if not text.endswith("\n") or text.count("\n") != 1: return None
        value = json.loads(text[:-1])
        return value if json.dumps(value, separators=(",", ":"), ensure_ascii=False) == text[:-1] else None
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


# ADR-0013 Decision §7 Phase B: fixed, production-owned, unsearched,
# image-independent build-time driver constants mirroring
# platforms/genesis/machine/include/segarecomp/machine/genesis/frontend.hpp's own
# m68k_discovery_max_seed_entries / m68k_expansion_max_rounds. Neither is
# derived from any ROM and neither is a retuning of the CPU-side per-seed-walk
# ceiling (m68k_discovery_max_instructions), which this driver never touches.
# SEG-007-T167 raised both 4 -> 5 (ADR-0013 Decision §7a amendment) from
# re-confirmed executed evidence. SEG-007-T170 raised both 5 -> 8 (ADR-0013
# Decision §7b amendment) from re-confirmed executed evidence; kept
# synchronized with the compiled header constants above.
M68K_DISCOVERY_MAX_SEED_ENTRIES = 8
M68K_EXPANSION_MAX_ROUNDS = 8

# ADR-0013 Decision §7c: the outcome reached when a Phase-B invocation
# consumes its per-invocation batch budget (either constant above) while
# pending expansion work remains -- i.e. the loop was still making progress,
# not stuck on a duplicate boundary. This is resumable driver state, never a
# target-program frontier: a later invocation started with `--checkpoint`
# continues from exactly the persisted confirmed-root set. It replaces the
# prior `expansion_seed_limit_reached` / `expansion_round_limit_reached`
# names for both budget dimensions -- both are the same "batch exhausted,
# resume me" outcome now that the seed/round constants are a per-invocation
# batch budget rather than a lifetime cap.
PHASE_B_BATCH_COMPLETE = "phase_b_batch_complete"

# ADR-0013 Decision §7c: when `--checkpoint` is supplied, `main` automatically
# re-invokes `run_expansion_loop` again with the freshly-saved checkpoint's
# seeds as `initial_seeds` whenever one batch ends in `PHASE_B_BATCH_COMPLETE`,
# so a canonical caller never has to notice or manually re-invoke on that
# outcome. This constant is ONLY a defensive safety net against a genuine
# infinite-loop bug (e.g. a driver defect that keeps confirming "progress"
# forever) -- it is not a new lifetime cardinality on the confirmed-root set
# or on the number of Phase-B rounds a discovery session may ever perform.
# The value is chosen generously high (an order of magnitude above any
# currently realistic route's own batch count) so it is never the actual
# limiting factor for any currently realistic Sonic route.
MAX_OUTER_RESUME_BATCHES = 64

# ADR-0013 Decision §7c checkpoint schema. `seeds` is the ordered, deduplicated
# `runtime_confirmed` promoted-root list (never including the always-present
# reset entry). The binding header (`rom_sha256`, `hints_path`, `hints_sha256`)
# must match the current invocation's actual inputs exactly, or the persisted
# state is fail-closed discarded and the invocation starts a fresh round 1.
CHECKPOINT_SCHEMA_VERSION = 1
_CHECKPOINT_KEYS = ["schema_version", "rom_sha256", "hints_path", "hints_sha256", "seeds"]


def hints_sha256(hints_path: str | None) -> str | None:
    if hints_path is None:
        return None
    try:
        return hashlib.sha256(pathlib.Path(hints_path).read_bytes()).hexdigest()
    except OSError:
        return None


def _valid_checkpoint_seed_list(seeds: object) -> bool:
    if not isinstance(seeds, list):
        return False
    seen = set()
    for seed in seeds:
        if not isinstance(seed, str) or len(seed) != 8 or any(c not in "0123456789abcdef" for c in seed):
            return False
        if seed in seen:
            return False
        seen.add(seed)
    return True


def load_checkpoint(path: pathlib.Path, digest: str, hints_path: str | None) -> list[int]:
    """Load and validate a persisted ADR-0013 Decision §7c checkpoint.

    Returns the persisted seed list (as ints, in order) only when every
    binding field matches this invocation's actual inputs exactly. Any
    missing file, unparseable JSON, wrong type, schema mismatch, or binding
    mismatch (different ROM or different external-hints path/content) fails
    closed to an empty list -- a fresh round 1 -- and never silently applies
    persisted roots to a different ROM/config.
    """
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError:
        return []
    try:
        state = json.loads(raw)
    except json.JSONDecodeError:
        return []
    if not isinstance(state, dict) or list(state) != _CHECKPOINT_KEYS:
        return []
    if (state.get("schema_version") != CHECKPOINT_SCHEMA_VERSION or
            state.get("rom_sha256") != digest or
            state.get("hints_path") != hints_path or
            state.get("hints_sha256") != hints_sha256(hints_path) or
            not _valid_checkpoint_seed_list(state.get("seeds"))):
        return []
    return [int(seed, 16) for seed in state["seeds"]]


def save_checkpoint(path: pathlib.Path, digest: str, hints_path: str | None, seeds: list[int]) -> None:
    state = {
        "schema_version": CHECKPOINT_SCHEMA_VERSION,
        "rom_sha256": digest,
        "hints_path": hints_path,
        "hints_sha256": hints_sha256(hints_path),
        "seeds": [f"{seed:08x}" for seed in seeds],
    }
    path.write_text(json.dumps(state, separators=(",", ":")) + "\n", encoding="utf-8")


COMPILE_JOBS_ENV = "SEGARECOMP_COMPILE_JOBS"
_compile_jobs_override: int | None = None


def default_compile_jobs() -> int:
    """SEG-022-T004: bounded compile concurrency. Default min(4, CPU count): safe on a modest
    machine (each -O2 TU compile is memory heavy). Override: --compile-jobs or SEGARECOMP_COMPILE_JOBS."""
    if _compile_jobs_override is not None:
        return max(1, min(_compile_jobs_override, 64))
    raw = os.environ.get(COMPILE_JOBS_ENV)
    if raw:
        try:
            return max(1, min(int(raw), 64))
        except ValueError:
            pass
    return max(1, min(4, os.cpu_count() or 1))


# SEG-022-T012: optional content-addressed cache of compiled generated-TU objects. Off unless
# --object-cache-dir or SEGARECOMP_OBJECT_CACHE_DIR names a directory; correctness never depends
# on it (a disabled, cold, warm, corrupt, or mismatched cache all yield the same objects, the latter
# by recompiling). The cache holds host objects compiled from generated C, so it inherits the
# out-dir's local/ignored-only status for commercial inputs (never commit or publish it).
OBJECT_CACHE_ENV = "SEGARECOMP_OBJECT_CACHE_DIR"
OBJECT_CACHE_MAX_BYTES_ENV = "SEGARECOMP_OBJECT_CACHE_MAX_BYTES"
OBJECT_CACHE_SCHEMA = b"segarecomp-generated-object-cache-v1"
_OBJECT_CACHE_MAGIC = b"SEGOBJ1\n"
_OBJECT_CACHE_DEFAULT_MAX_BYTES = 4 << 30
# Environment that can change what the compiler driver compiles/targets without appearing in argv.
_OBJECT_CACHE_ENV_KEYS = ("SDKROOT", "DEVELOPER_DIR", "MACOSX_DEPLOYMENT_TARGET", "CPATH", "C_INCLUDE_PATH",
                          "OBJC_INCLUDE_PATH", "CCC_OVERRIDE_OPTIONS", "COMPILER_PATH", "GCC_EXEC_PREFIX",
                          "SOURCE_DATE_EPOCH")
_object_cache_dir_override: "pathlib.Path | None" = None
_object_cache_stats: "dict[str, int]" = {}
_compiler_identity_memo: "dict[tuple, bytes | None]" = {}


def object_cache_dir() -> "pathlib.Path | None":
    if _object_cache_dir_override is not None:
        return _object_cache_dir_override
    raw = os.environ.get(OBJECT_CACHE_ENV)
    return pathlib.Path(raw).resolve() if raw else None


def _object_cache_max_bytes() -> int:
    raw = os.environ.get(OBJECT_CACHE_MAX_BYTES_ENV)
    try:
        return max(0, int(raw)) if raw else _OBJECT_CACHE_DEFAULT_MAX_BYTES
    except ValueError:
        return _OBJECT_CACHE_DEFAULT_MAX_BYTES


_object_cache_lock = threading.Lock()


def _count(stat: str) -> None:
    with _object_cache_lock:
        _object_cache_stats[stat] = _object_cache_stats.get(stat, 0) + 1


def compiler_identity(compiler: str, cwd: pathlib.Path) -> "bytes | None":
    """Compiler identity/version and target triple plus compile-affecting environment. None (the
    job is then compiled uncached) when the driver cannot report them."""
    env_part = tuple((k, os.environ.get(k)) for k in _OBJECT_CACHE_ENV_KEYS)
    resolved = shutil.which(compiler) or compiler
    memo_key = (compiler, os.path.realpath(resolved), env_part)
    if memo_key in _compiler_identity_memo:
        return _compiler_identity_memo[memo_key]
    identity: "bytes | None" = None
    try:
        version = subprocess.run([compiler, "--version"], capture_output=True, cwd=cwd, timeout=60)
        machine = subprocess.run([compiler, "-dumpmachine"], capture_output=True, cwd=cwd, timeout=60)
        if version.returncode == 0 and machine.returncode == 0 and machine.stdout.strip():
            identity = b"\0".join([os.path.realpath(resolved).encode(), version.stdout, version.stderr,
                                   machine.stdout, json.dumps(env_part).encode()])
    except (OSError, subprocess.SubprocessError):
        identity = None
    _compiler_identity_memo[memo_key] = identity
    return identity


def object_cache_key(argv: "list[str]", src: pathlib.Path, cwd: pathlib.Path) -> "str | None":
    """Key = schema + compiler identity/target + exact compile argv + cwd + source path + digest of
    the full preprocessed TU (`-E -dD`: line markers and macro definitions kept), which covers the TU content and every
    included header (content and resolved path). None = do not use the cache for this job."""
    identity = compiler_identity(argv[0], cwd)
    if identity is None:
        return None
    digest = hashlib.sha256()
    try:
        proc = subprocess.Popen(argv + ["-E", "-dD", str(src)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=cwd)
        assert proc.stdout is not None
        for chunk in iter(lambda: proc.stdout.read(1 << 20), b""):
            digest.update(chunk)
        proc.stdout.close()
        if proc.wait() != 0:
            return None
    except OSError:
        return None
    key = hashlib.sha256()
    for field in (OBJECT_CACHE_SCHEMA, identity, json.dumps(argv).encode(), str(cwd.resolve()).encode(),
                  str(src.resolve()).encode(), digest.digest()):
        key.update(len(field).to_bytes(8, "little"))
        key.update(field)
    return key.hexdigest()


def _object_cache_entry(cache: pathlib.Path, key: str) -> pathlib.Path:
    return cache / key[:2] / (key + ".obj")


def object_cache_load(cache: pathlib.Path, key: str, dest: str) -> bool:
    """Copy a verified entry to dest. Any unreadable/truncated/corrupt entry is removed (fail safe)."""
    entry = _object_cache_entry(cache, key)
    try:
        blob = entry.read_bytes()
    except OSError:
        return False
    head = len(_OBJECT_CACHE_MAGIC) + 64 + 1
    if (len(blob) > head and blob.startswith(_OBJECT_CACHE_MAGIC) and blob[head - 1:head] == b"\n"
            and hashlib.sha256(blob[head:]).hexdigest().encode() == blob[len(_OBJECT_CACHE_MAGIC):head - 1]):
        try:
            pathlib.Path(dest).write_bytes(blob[head:])
            os.utime(entry)  # recency for size-bounded eviction
        except OSError:
            return False
        return True
    _count("rejected")
    entry.unlink(missing_ok=True)
    return False


def object_cache_store(cache: pathlib.Path, key: str, obj: str) -> None:
    """Best effort; atomic replace so a concurrent reader never sees a partial entry."""
    try:
        data = pathlib.Path(obj).read_bytes()
        entry = _object_cache_entry(cache, key)
        entry.parent.mkdir(parents=True, exist_ok=True)
        tmp = entry.with_name(f"{entry.name}.{os.getpid()}.{threading.get_ident()}.tmp")
        tmp.write_bytes(_OBJECT_CACHE_MAGIC + hashlib.sha256(data).hexdigest().encode() + b"\n" + data)
        os.replace(tmp, entry)
        _count("stores")
    except OSError:
        _count("store_failed")


def object_cache_trim(cache: pathlib.Path) -> None:
    """Bound disk use: evict least-recently-used entries beyond the byte cap."""
    try:
        entries = [(p.stat().st_mtime_ns, p.stat().st_size, p) for p in cache.glob("*/*.obj")]
    except OSError:
        return
    total = sum(size for _, size, _ in entries)
    limit = _object_cache_max_bytes()
    for _, size, path in sorted(entries, key=lambda e: (e[0], str(e[2]))):
        if total <= limit:
            break
        path.unlink(missing_ok=True)
        total -= size
        _count("evicted")


def compile_objects(jobs: "list[tuple[list[str], pathlib.Path]]", cwd: pathlib.Path,
                    tmp: pathlib.Path) -> "tuple[list[str], str | None]":
    """Compile independent TUs concurrently (bounded). jobs = (argv before `-c`, source).
    Object paths and link order follow job order, never completion order. On the first failure
    pending jobs are cancelled and the diagnostics of the lowest-index failed job are returned.
    SEG-022-T012: with an object cache configured, a job whose key matches a verified entry reuses
    that object instead of compiling; misses compile normally and store only on success."""
    import concurrent.futures as cf
    objects = [str(tmp / f"{i}.o") for i in range(len(jobs))]
    cache = object_cache_dir()
    if cache is not None:
        try:
            cache.mkdir(parents=True, exist_ok=True)
        except OSError:
            cache = None

    with _object_cache_lock:
        before = dict(_object_cache_stats)

    def run(i: int):
        argv, src = jobs[i]
        key = object_cache_key(argv, src, cwd) if cache is not None else None
        if cache is not None and key is None:
            _count("uncacheable")
        if key is not None and object_cache_load(cache, key, objects[i]):
            _count("hits")
            return subprocess.CompletedProcess(argv, 0, "", "")
        result = subprocess.run(argv + ["-c", "-o", objects[i], str(src)], text=True, capture_output=True, cwd=cwd)
        if key is not None:
            _count("misses")
            if result.returncode == 0:
                object_cache_store(cache, key, objects[i])
        return result

    failures: dict[int, str] = {}
    with cf.ThreadPoolExecutor(max_workers=min(default_compile_jobs(), max(1, len(jobs)))) as pool:
        futures = {i: pool.submit(run, i) for i in range(len(jobs))}
        pending = set(futures.values())
        index_of = {f: i for i, f in futures.items()}
        while pending and not failures:
            done, pending = cf.wait(pending, return_when=cf.FIRST_COMPLETED)
            for f in done:
                res = f.result()
                if res.returncode != 0:
                    failures[index_of[f]] = res.stderr
        if failures:
            for f in pending:
                f.cancel()
    if cache is not None:
        object_cache_trim(cache)
        with _object_cache_lock:
            delta = {k: v - before.get(k, 0) for k, v in _object_cache_stats.items() if v != before.get(k, 0)}
        sys.stderr.write("generated object cache: " + " ".join(f"{k}={delta[k]}" for k in sorted(delta)) + "\n")
    if failures:
        return [], failures[min(failures)]
    return objects, None


def resolve_profile(args) -> str:
    if args.build_profile != "auto":
        return args.build_profile
    return "optimized" if args.viewer else "quick"


def generate_and_compile(emitter_command: list[str], compiler: pathlib.Path, root: pathlib.Path,
                         out_dir: pathlib.Path, debug: "bool | str",
                         viewer_sdl3: tuple[list[str], list[str]] | None = None,
                         ) -> tuple[int, bytes | None, pathlib.Path | None]:
    source = out_dir / "bridge.generated.c"
    executable = out_dir / "bridge"
    # SEG-022-T002: the emitter streams the generated C straight to `source` (written as
    # `<source>.partial` and atomically renamed only after complete success), so neither the
    # emitter nor this bridge ever holds the whole program. Its stdout stays empty; only the
    # (small, separately retained) stderr diagnostics are captured. A rejection or partial
    # write exits non-zero with no `source` left behind and fails closed.
    source.unlink(missing_ok=True)
    # SEG-022-T003: the emitter may instead write a bounded deterministic set of translation units
    # into `shard_dir` (a large program) and reports them in `bridge_generated.units`; a small
    # program still produces the single `source`. Stale output of either shape is removed first.
    shard_dir = out_dir / "generated"
    shutil.rmtree(shard_dir, ignore_errors=True)
    try:
        generated = subprocess.run(emitter_command + ["--generated-c-output", str(source),
                                                      "--generated-c-shard-dir", str(shard_dir)],
                                   text=True, capture_output=True, cwd=root)
    except OSError as error:
        sys.stderr.write(f"cannot run emitter: {error}\n")
        return 1, None, None
    if generated.returncode == 0 and not source.exists() and generated.stdout \
            and not generated.stdout.startswith("/* translation rejected:"):
        # Project-authored emitter proxies (tests) predate the streaming option and write the
        # program to stdout; the real emitter never takes this branch.
        source.write_text(generated.stdout, encoding="utf-8", newline="\n")
    manifest = shard_dir / "bridge_generated.units"
    sharded = generated.returncode == 0 and manifest.is_file()
    if generated.returncode != 0 or generated.stdout.startswith("/* translation rejected:") or not (sharded or source.is_file()):
        source.unlink(missing_ok=True)
        pathlib.Path(str(source) + ".partial").unlink(missing_ok=True)
        shutil.rmtree(shard_dir, ignore_errors=True)
        sys.stderr.write(generated.stderr)
        return 1, None, None
    sources = [source]
    if sharded:
        # Manifest order is deterministic: the main TU (the only one defining `main`) first, then sorted.
        sources = [shard_dir / line for line in manifest.read_text(encoding="utf-8").splitlines() if line]
        if not sources or not all(item.is_file() for item in sources):
            sys.stderr.write("generated-C translation-unit manifest is inconsistent\n")
            shutil.rmtree(shard_dir, ignore_errors=True)
            return 1, None, None
    # SEG-007-T180 / ADR-0026: capture the emitter's normalized offline-inventory
    # stitch metrics line (counts only, never a raw address) fully in-process from
    # the stderr string. Nothing is written into `out_dir`: that surface is the
    # compare-runs privacy boundary and carries an exact expected-artifacts
    # allowlist ({"bridge.generated.c", "bridge"}).
    _merged_metrics = parse_offline_inventory_stitch_metrics(generated.stderr)
    _merged_metrics.update(parse_offline_inventory_partition_metrics(generated.stderr))
    _merged_metrics.update(parse_offline_inventory_emission_metrics(generated.stderr))
    _OFFLINE_INVENTORY_STITCH_METRICS_BY_DIR[str(out_dir.resolve())] = _merged_metrics
    try:
        compile_flags = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
        # SEG-018-T005: profiles: debug (-O0 -g), optimized (-O2), quick (-O0, no -g).
        # `debug` accepts a profile name; legacy bool callers: True=debug, False=quick.
        profile = "debug" if debug is True else ("quick" if debug is False else debug)
        compile_flags += {"debug": ["-O0", "-g"], "optimized": ["-O2"], "quick": ["-O0"]}[profile]
        if viewer_sdl3 is not None:
            return _compile_viewer_executable(compile_flags, viewer_sdl3, root, sources, executable,
                                              None)
        runtime_flags = ["-I", str(root / "platforms" / "genesis" / "runtime")]
        if not sharded:
            compile_result = subprocess.run(compile_flags + runtime_flags + [
                "-o", str(executable), str(source), str(root / "platforms" / "genesis" / "runtime" / "runtime.c")],
                text=True, capture_output=True, cwd=root)
        else:
            # Bounded parallel per-TU compile (SEG-022-T004) then link. Objects live in a temp dir,
            # never in out_dir, which is the compare-runs artifact surface. Link order = job order.
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                base = compile_flags + runtime_flags + ["-I", str(shard_dir)]
                units = sources + [root / "platforms" / "genesis" / "runtime" / "runtime.c"]
                objects, failure = compile_objects([(base, u) for u in units], root, pathlib.Path(tmp))
                if failure is not None:
                    sys.stderr.write(failure)
                    return 2, None, None
                compile_result = subprocess.run([compile_flags[0], "-o", str(executable)] + objects,
                                                text=True, capture_output=True, cwd=root)
    except OSError as error:
        sys.stderr.write(f"cannot run C compiler: {error}\n")
        return 2, None, None
    if compile_result.returncode != 0:
        sys.stderr.write(compile_result.stderr)
        return 2, None, None
    return 0, None, executable


def find_sdl3() -> tuple[list[str], list[str]] | None:
    """SEG-007-T254: locate an installed SDL3 (compile flags, link flags) or None.

    Order: SDL3_PREFIX env, then common prefixes. Only a prefix that has both the
    SDL3 header and a libSDL3 shared/static library is accepted.
    """
    prefixes = [p for p in (os.environ.get("SDL3_PREFIX"), "/opt/homebrew", "/usr/local", "/usr") if p]
    for prefix in prefixes:
        base = pathlib.Path(prefix)
        if not (base / "include" / "SDL3" / "SDL.h").is_file():
            continue
        libdir = base / "lib"
        if any(libdir.glob("libSDL3.*")):
            return ["-I", str(base / "include")], ["-L", str(libdir), "-Wl,-rpath," + str(libdir), "-lSDL3"]
    return None


def _compile_viewer_executable(compile_flags: list[str], sdl3: tuple[list[str], list[str]],
                               root: pathlib.Path, sources: "list[pathlib.Path]", executable: pathlib.Path,
                               generated_bytes: "bytes | None") -> tuple[int, bytes | None, pathlib.Path | None]:
    """Viewer-mode build of the UNMODIFIED generated C. The generated source alone is
    compiled with -Dgenesis_runtime_run=genesis_viewer_hook_run so its main() hands its
    own runtime/dispatcher/allowance to the viewer hook; every other object is compiled
    without the macro. Objects live in a temp dir (never in out_dir)."""
    import tempfile
    runtime_dir = root / "platforms" / "genesis" / "runtime"
    viewer_dir = root / "platforms" / "genesis" / "viewer"
    includes = ["-I", str(runtime_dir), "-I", str(viewer_dir)] + sdl3[0]
    others = [runtime_dir / "runtime.c", runtime_dir / "vdp_render.c", viewer_dir / "viewer.c",
              viewer_dir / "viewer_sdl3.c", viewer_dir / "viewer_main_hook.c"]
    with tempfile.TemporaryDirectory() as tmp:
        # sources[0] is the main TU (the only one whose main() calls genesis_runtime_run).
        jobs = [(sources[0], ["-Dgenesis_runtime_run=genesis_viewer_hook_run"])] + [(u, ["-I", str(sources[0].parent)]) for u in sources[1:]] \
            + [(o, []) for o in others]
        objects, failure = compile_objects([(compile_flags + extra + includes, src) for src, extra in jobs],
                                           root, pathlib.Path(tmp))
        if failure is not None:
            sys.stderr.write(failure)
            return 2, None, None
        link = subprocess.run([compile_flags[0], "-o", str(executable)] + objects + sdl3[1],
                              text=True, capture_output=True, cwd=root)
        if link.returncode != 0:
            sys.stderr.write(link.stderr)
            return 2, None, None
    return 0, generated_bytes, executable


def run_viewer(executable: pathlib.Path, root: pathlib.Path, instruction_budget: int,
               unthrottled: bool, slice_dispatches: int | None) -> int:
    """SEG-007-T254: launch the viewer-mode generated executable with inherited stdio.
    Viewer options travel via env so the generated argv ABI is unchanged."""
    env = dict(os.environ)
    if unthrottled:
        env["SEGARECOMP_VIEWER_UNTHROTTLED"] = "1"
    if slice_dispatches is not None:
        env["SEGARECOMP_VIEWER_SLICE"] = str(slice_dispatches)
    return subprocess.run([str(executable), "--instruction-budget", str(instruction_budget)],
                          cwd=root, env=env).returncode


def run_expansion_loop(base_emitter_command: list[str], compiler: pathlib.Path, root: pathlib.Path,
                       out_dir: pathlib.Path, digest: str, initial_seeds: list[int] | None = None,
                       instruction_budget: int | None = None,
                       ) -> tuple[int, dict | None, bytes | None, pathlib.Path | None, dict, str, list[int]]:
    """ADR-0013 Decision §7/§7c build/execute/expand loop, one Phase-B batch.

    `initial_seeds` is the persistent, monotonically growing runtime-confirmed
    root set `R` accumulated by prior invocations (empty on a fresh session).
    Round 1's seed set is `{reset entry} ∪ initial_seeds`. Every subsequent
    round adds the prior round's own runtime-confirmed discovery_prefix_boundary
    address, extracted privately from that round's full/ephemeral report only,
    never the sanitized one. This single invocation promotes at most
    `M68K_DISCOVERY_MAX_SEED_ENTRIES` NEW roots and performs at most
    `M68K_EXPANSION_MAX_ROUNDS` rounds (Decision §7c: a per-invocation batch
    budget, not a lifetime ceiling on the confirmed-root set).

    Returns (status, final_sanitized_report_or_None, final_full_bytes_or_None,
    final_executable_or_None, summary, message, final_seeds). `summary` carries
    only normalized fields -- round count and a driver/stop class name -- never
    a raw address. `final_seeds` is the complete confirmed-root set (initial
    plus every root newly promoted in this invocation) and is always returned,
    even on a terminal failure, so a caller can persist exactly the roots that
    were actually confirmed before the failure.
    """
    seeds: list[int] = list(initial_seeds) if initial_seeds else []
    newly_promoted = 0
    report: dict | None = None
    full_bytes: bytes | None = None
    executable: pathlib.Path | None = None
    for round_number in range(1, M68K_EXPANSION_MAX_ROUNDS + 1):
        round_command = list(base_emitter_command)
        for seed in seeds:
            round_command += ["--analysis-seed", f"{seed:08x}"]
        round_out_dir = out_dir / f"round-{round_number}"
        round_out_dir.mkdir(parents=True, exist_ok=True)
        status, _, executable = generate_and_compile(round_command, compiler, root, round_out_dir, True)
        if status:
            # ADR-0013 Decision §7 Phase B: a promoted seed can legitimately
            # turn a previously-unresolved sibling candidate frontier address
            # (a DIFFERENT seed's own best-effort exploration) into a
            # retained/admitted instruction of the new seed's own walk;
            # discover_m68k_general_startup's existing, unmodified
            # runtime_frontier_eligible / retained-vs-sibling-address
            # invariant then fails that whole build-time translation closed
            # (status 1) rather than emit an unrepresentable partial program.
            # This is the same class of terminal, acceptable, fail-closed
            # outcome ADR-0013 Decision §7 Phase A's own probe-failure
            # rejection already is -- never a driver bug to work around.
            driver_result = "build_time_translation_rejected" if status == 1 else "strict_c11_compile_failed"
            return status, None, None, None, {"rounds": round_number, "driver_result": driver_result}, "", seeds
        assert executable is not None
        status, report, full_bytes, message, _ = run_bridge(executable, root, full_pipe=True,
                                                            instruction_budget=instruction_budget)
        if status:
            return status, None, None, None, {"rounds": round_number, "driver_result": "run_failed"}, message, seeds
        sha_status = report_sha_status(report, digest)
        if sha_status:
            return sha_status, None, None, None, {"rounds": round_number}, "", seeds
        if not valid_sanitized(report, digest):
            return 5, None, None, None, {"rounds": round_number}, "", seeds
        full = parse_canonical_full(full_bytes if full_bytes is not None else b"")
        if full is None or report_sha_status(full, digest) or not valid_full(full, report, digest):
            return 5, None, None, None, {"rounds": round_number}, "", seeds

        seed_count = len(seeds) + 1
        if report["result"] == "completed":
            return (0, report, full_bytes, executable,
                    {"rounds": round_number, "driver_result": "completed", "seed_count": seed_count}, "", seeds)
        # SEG-007-T252 / ADR-0040: a runner resource-limit exhaustion is a
        # sibling top-level result, never a `stop_class` value -- normalize
        # its own driver_result label distinctly rather than surfacing a
        # misleading `None` (this loop's expansion budget is unrelated to
        # M68K_EXPANSION_MAX_ROUNDS; hitting it here just means the runner's
        # own dispatch allowance for this one invocation was exhausted).
        if report["result"] == "runner_resource_limit":
            return (0, report, full_bytes, executable,
                    {"rounds": round_number, "driver_result": "runner_resource_limit", "seed_count": seed_count}, "",
                    seeds)
        if report["stop_class"] != "discovery_prefix_boundary":
            return (0, report, full_bytes, executable,
                    {"rounds": round_number, "driver_result": report["stop_class"], "seed_count": seed_count}, "",
                    seeds)

        # ADR-0013 Decision §7 Phase B: privately extract the boundary address
        # from the full/ephemeral report only -- the sanitized report never
        # carries one. This value is ephemeral build-time driver state; it is
        # never printed to stdout and never placed in durable evidence.
        provenance = full.get("provenance")
        instruction = provenance.get("instruction") if isinstance(provenance, dict) else None
        address_hex = instruction.get("source_address") if isinstance(instruction, dict) else None
        if not isinstance(address_hex, str) or not address_hex.startswith("0x"):
            return 5, None, None, None, {"rounds": round_number}, "malformed boundary provenance\n", seeds
        boundary_address = int(address_hex, 16)

        # A boundary address is always strictly ahead of the seed that
        # discovered it (at least m68k_discovery_max_instructions admitted
        # instructions past it), so it can structurally never equal the fixed
        # reset entry itself; tracking only the previously promoted seeds
        # (persisted plus newly promoted this invocation) is therefore the
        # complete membership test for "already a member of R" per ADR-0013
        # Decision §7/§7c.
        if boundary_address in seeds:
            return (0, report, full_bytes, executable,
                    {"rounds": round_number, "driver_result": "expansion_no_progress", "seed_count": seed_count},
                    "", seeds)
        if newly_promoted == M68K_DISCOVERY_MAX_SEED_ENTRIES:
            # Decision §7c: this invocation's batch budget of NEWLY promoted
            # roots is consumed while valid pending expansion work remains
            # (a new, distinct boundary was just confirmed). This is a
            # resumable outcome, never a target-program frontier.
            return (0, report, full_bytes, executable,
                    {"rounds": round_number, "driver_result": PHASE_B_BATCH_COMPLETE,
                     "seed_count": seed_count}, "", seeds)
        seeds.append(boundary_address)
        newly_promoted += 1
    # Decision §7c: the per-invocation round budget was consumed while the
    # final round still confirmed a boundary worth continuing from -- also a
    # resumable batch-complete outcome, not a lifetime round cap.
    return (0, report, full_bytes, executable,
            {"rounds": M68K_EXPANSION_MAX_ROUNDS, "driver_result": PHASE_B_BATCH_COMPLETE,
             "seed_count": len(seeds) + 1}, "", seeds)


_OFFLINE_INVENTORY_STITCH_METRICS_BY_DIR: dict[str, dict] = {}
_OFFLINE_METRIC_UNSIGNED_MAX = UINT32_MAX
_OFFLINE_METRIC_SIGNED_MAX = (1 << 63) - 1
_OFFLINE_METRIC_SIGNED_MIN_MAGNITUDE = 1 << 63


def _parse_bounded_metric_integer(value: str, signed: bool) -> int | None:
    """Parse one canonical decimal metric without feeding unbounded text to int()."""
    negative = signed and value.startswith("-")
    digits = value[1:] if negative else value
    if (not digits or any(digit < "0" or digit > "9" for digit in digits) or
            (len(digits) > 1 and digits.startswith("0")) or (negative and digits == "0")):
        return None
    maximum = (_OFFLINE_METRIC_SIGNED_MIN_MAGNITUDE if negative else
               _OFFLINE_METRIC_SIGNED_MAX if signed else _OFFLINE_METRIC_UNSIGNED_MAX)
    maximum_text = str(maximum)
    if len(digits) > len(maximum_text) or (len(digits) == len(maximum_text) and digits > maximum_text):
        return None
    parsed = int(digits)
    return -parsed if negative else parsed


def _parse_marked_metrics_line(
        stderr_text: str, marker: str, signed_integer_keys: frozenset[str] = frozenset()) -> dict:
    """Parse a normalized `<marker>key=value ...` stderr line into a dict.

    Shared by every `parse_offline_inventory_*_metrics` function below: each
    emitter/frontend metrics line uses the identical `key=value` shape, so one
    generic tokenizer covers all of them. Values are non-negative integer
    counts/flags or bracketed lists of counts except for explicitly named
    signed scalar deltas. Signed values accept only the canonical optional
    leading minus form; unsigned metrics keep their stricter handling. No
    value is a raw address. Returns an empty dict when no line with `marker`
    was present.
    """
    for line in stderr_text.splitlines():
        index = line.find(marker)
        if index == -1:
            continue
        metrics: dict[str, int | list[int]] = {}
        for token in line[index + len(marker):].split():
            key, sep, value = token.partition("=")
            parsed_scalar = _parse_bounded_metric_integer(value, key in signed_integer_keys) if sep else None
            if parsed_scalar is not None:
                metrics[key] = parsed_scalar
            elif sep and value.startswith("[") and value.endswith("]"):
                items = value[1:-1].split(",") if len(value) > 2 else []
                parsed_items = [_parse_bounded_metric_integer(item, False) for item in items]
                if all(item is not None for item in parsed_items):
                    metrics[key] = parsed_items
        return metrics
    return {}


def parse_offline_inventory_stitch_metrics(stderr_text: str) -> dict:
    """Parse the emitter's normalized offline-inventory stitch metrics line.

    SEG-007-T180 / ADR-0026: the Genesis frontend prints one
    `segarecomp: offline inventory stitch: key=value ...` line to stderr when a
    build supplies `external_code_entry_candidates`. Every value is a count
    (candidates, admitted units, per-reason rejections, stitched direct edges,
    overlap agree/conflict, and local/aggregate discovery sizes) -- never a raw
    address. Returns an empty dict when no such line was present.
    """
    return _parse_marked_metrics_line(stderr_text, "segarecomp: offline inventory stitch: ")


def parse_offline_inventory_partition_metrics(stderr_text: str) -> dict:
    """Parse the emitter's normalized offline-inventory partition metrics line.

    SEG-007-T183 / ADR-0028 §8: the Genesis frontend prints one
    `segarecomp: offline inventory partition: key=value ...` line to stderr,
    guarded on the same non-empty-inventory condition as the stitch metrics
    line above, once the semantic-partition-boundary/bounded-diagnostic-
    frontier-projection metrics for a partial promotion are known
    (`semantic_partition_boundary_count`,
    `unresolved_semantic_frontier_count_before_bounding`,
    `diagnostic_frontier_count_after_bounding`,
    `residual_frontier_obligation_count`,
    `retained_block_count_before_pruning`, `retained_block_count_after_pruning`,
    `ingress_retained`). `adr0038_retained_block_delta` is the sole signed
    value; all other values remain unsigned counts or 0/1 flags. No value is a
    raw address. Returns an empty dict when no such line was present.
    """
    return _parse_marked_metrics_line(
        stderr_text, "segarecomp: offline inventory partition: ",
        frozenset({"adr0038_retained_block_delta"}))


def parse_offline_inventory_emission_metrics(stderr_text: str) -> dict:
    """Parse the emitter's normalized offline-inventory emission metrics line.

    SEG-007-T181 / ADR-0027 §6 (wiring the deferred ADR-0026 §5 metrics): the C4
    emitter prints one `segarecomp: offline inventory emission: key=value ...`
    line to stderr once the final `EmittedCodeAddressSet` is known, guarded on a
    non-empty offline inventory. Values are counts only (emitted_block_count,
    emitted_code_address_count) -- never a raw address. Returns an empty dict
    when no such line was present.
    """
    return _parse_marked_metrics_line(stderr_text, "segarecomp: offline inventory emission: ")


def offline_inventory_stitch_metrics(directory: pathlib.Path) -> dict:
    """Return the in-process stitch metrics captured for `directory`'s build.

    SEG-007-T180 / ADR-0026: metrics are parsed from the emitter's stderr string
    at generation time (see `generate_and_compile`) and cached by resolved output
    directory. No artifact is written into the compared out-dir surface, so the
    compare-runs expected-artifacts allowlist is unaffected. Returns an empty
    dict for every build without an offline inventory.
    """
    return _OFFLINE_INVENTORY_STITCH_METRICS_BY_DIR.get(str(directory.resolve()), {})


def ephemeral_frontier(full: dict, executable: pathlib.Path,
                        recent_pc_history: list[str] | None = None) -> dict:
    """Return bounded private-session attribution, never durable evidence."""
    # SEG-007-T252 / ADR-0040 correction: surface the bounded 64-entry
    # diagnostic PC history ONLY for a runner-resource-limit frontier (a
    # genuine guest semantic stop already has its own precise PC/provenance
    # fields below instead). `provenance` is always null for this result kind
    # (see genesis_write_full_report), so this branch must come BEFORE the
    # `isinstance(provenance, dict)` early return below -- that early return
    # previously discarded the history entirely for the exact case it exists
    # to serve. The history no longer travels through the stable full-report
    # pipe at all (see genesis_write_full_report / valid_full) -- it arrives
    # ONLY via the separate ephemeral-report-fd transport parsed by
    # `parse_ephemeral_pc_history` and passed in here explicitly. Its absence
    # here is not an error, only a lack of history to report.
    if full.get("result") == "runner_resource_limit":
        frontier = {"report_kind": "ephemeral_frontier", "debug_binary": str(executable)}
        if isinstance(recent_pc_history, list) and recent_pc_history:
            frontier["recent_pc_history"] = recent_pc_history
        return frontier
    provenance = full.get("provenance")
    if not isinstance(provenance, dict):
        return {"report_kind": "ephemeral_frontier", "debug_binary": str(executable)}
    instruction = provenance.get("instruction") if provenance.get("has_instruction_provenance") else None
    access = None
    if provenance.get("has_access"):
        access = {
            "address": provenance.get("access_address"),
            "width": provenance.get("access_width"),
            "direction": provenance.get("access_direction"),
        }
    return {
        "report_kind": "ephemeral_frontier",
        "runtime_pc": full.get("runtime", {}).get("pc") if isinstance(full.get("runtime"), dict) else None,
        "instruction": instruction,
        "access": access,
        "debug_binary": str(executable),
    }


# SEG-020-T007 / ADR-0042 sections 1, 10: the single private diagnosis assembly. It only *combines*
# already-existing evidence (sanitized stop category, ephemeral frontier/provenance/history and, on
# request, the divergence report produced by tools/m68k_first_divergence.py or
# tools/genesis_device_divergence.py) and derives one reduced, non-reconstructable projection.
# Parse failure of an optional input never fails a run. Nothing here reaches generated code.
DIAGNOSIS_SCHEMA = 1
_DIVERGENCE_DOMAINS = ("cpu", "device", "none")
_DIVERGENCE_CLASSES = ("device_command", "device_state")
_DIVERGENCE_RESULTS = ("diverged", "no_divergence", "unsupported_for_comparison")
# Closed vocabulary of field classes emitted by tools/m68k_first_divergence.py (T005) and
# tools/genesis_device_divergence.py (T006). Anything else reduces to exactly "other".
_FIELD_PLAIN = frozenset(
    ["d%d" % i for i in range(8)] + ["a%d" % i for i in range(8)] +
    ["usp", "sr", "pc", "boundary_presence", "boundary_ordinal", "unsupported", "device_boundary_presence",
     "device_boundary_ordinal", "device_unsupported", "event:order", "event:vblank_raise", "event:irq_admit",
     "effect:trap"] +
    ["state:" + c for c in ("vdp_registers", "vdp_dma", "vdp_vram", "vdp_cram", "vdp_vsram", "interrupt", "psg",
                            "z80_bus", "z80_ram", "controller_io")])
_DEVICE_REGIONS = frozenset(("controller_io", "psg", "ym2612", "vdp", "z80_bus", "z80_ram_window"))
_ACCESS_WIDTHS = frozenset(("w1", "w2", "w4"))


def load_divergence_report(path: pathlib.Path) -> dict | None:
    """Read a divergence report; any missing/malformed input is 'unavailable', never an error."""
    try:
        report = json.loads(path.read_text())
    except (OSError, ValueError, RecursionError):
        return None
    if (not isinstance(report, dict) or report.get("domain") not in _DIVERGENCE_DOMAINS or
            report.get("result") not in _DIVERGENCE_RESULTS):
        return None
    return report


def durable_field_class(name: object) -> str:
    """Reduce a differing-field name to a closed class; never echoes any input substring."""
    if not isinstance(name, str):
        return "other"
    if name in _FIELD_PLAIN:
        return name
    match = re.match(r"^effect:write@[0-9A-Fa-f]{1,8}/(w[0-9]+)(?:#[0-9]+)?\Z", name)
    if match and match.group(1) in _ACCESS_WIDTHS:
        return "effect:write/" + match.group(1)
    match = re.match(r"^event:write@([a-z0-9_]+)/[0-9A-Fa-f]{1,8}/(w[0-9]+)(?:#[0-9]+)?\Z", name)
    if match and match.group(1) in _DEVICE_REGIONS and match.group(2) in _ACCESS_WIDTHS:
        return "event:write@%s/%s" % match.groups()
    return "other"


def durable_diagnosis(sanitized: dict, divergence: dict | None) -> dict:
    """Non-reconstructable projection safe for durable/CI output: classes and ordinals only."""
    result: dict = {"schema": DIAGNOSIS_SCHEMA, "report_kind": "diagnosis_classes",
                    "result": sanitized.get("result"), "stop_class": sanitized.get("stop_class"),
                    "diagnostic_category": sanitized.get("diagnostic_category")}
    if divergence is None:
        result["divergence"] = None
        return result
    projected: dict = {"result": divergence["result"], "domain": divergence["domain"]}
    if divergence.get("classification") in _DIVERGENCE_CLASSES:
        projected["classification"] = divergence["classification"]
    for key in ("last_matching_boundary", "first_differing_boundary"):
        if isinstance(divergence.get(key), int) and not isinstance(divergence.get(key), bool):
            projected[key] = divergence[key]
    fields = divergence.get("fields")
    if isinstance(fields, list):
        projected["differing_fields"] = sorted({durable_field_class(f.get("field")) for f in fields
                                                if isinstance(f, dict)})
    result["divergence"] = projected
    return result


def combined_diagnosis(sanitized: dict, frontier: dict, divergence: dict | None) -> dict:
    """Ephemeral one-stop report (private session only): stop + frontier + optional divergence."""
    return {"schema": DIAGNOSIS_SCHEMA, "report_kind": "ephemeral_diagnosis",
            "stop": {"result": sanitized.get("result"), "stop_class": sanitized.get("stop_class"),
                     "diagnostic_category": sanitized.get("diagnostic_category")},
            "frontier": frontier, "divergence": divergence}


def write_combined_diagnosis(divergence_path: str | None, sanitized: dict, frontier: dict) -> None:
    """Emit EPHEMERAL_DIAGNOSIS (private) and DIAGNOSIS_CLASSES (durable-safe) when requested."""
    if not divergence_path:
        return
    divergence = load_divergence_report(pathlib.Path(divergence_path))
    sys.stderr.write("EPHEMERAL_DIAGNOSIS " + json.dumps(
        combined_diagnosis(sanitized, frontier, divergence), sort_keys=True, separators=(",", ":")) + "\n")
    sys.stderr.write("DIAGNOSIS_CLASSES " + json.dumps(
        durable_diagnosis(sanitized, divergence), sort_keys=True, separators=(",", ":")) + "\n")


def default_segarecomp(root: pathlib.Path, os_name: str = os.name) -> pathlib.Path:
    suffix = ".exe" if os_name == "nt" else ""
    return root / "build" / "dev" / "apps" / "segarecomp" / f"segarecomp{suffix}"


def select_compiler(cli_cc: str | None, env_cc: str | None, which=shutil.which) -> str | None:
    """--cc, else $CC, else cc/clang/gcc on PATH; bare names resolve through PATH."""
    chosen = cli_cc or env_cc
    if chosen:
        return which(chosen) or chosen
    return next((found for found in map(which, ("cc", "clang", "gcc")) if found), None)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate, compile (strict C11) and run a Genesis generated-native program.",
        epilog="examples:\n"
               "  synthetic: python3 tools/genesis_startup_bridge.py --rom fixture.bin --entry 00000B00 --mode synthetic\n"
               "  viewer:    python3 tools/genesis_startup_bridge.py --rom games/<rom> --mode commercial "
               "--one-shot --diagnose-frontier --immutable-rom-aot --viewer\n"
               "--segarecomp defaults to the dev-preset build; --cc defaults to $CC, then cc/clang/gcc on PATH "
               "(pass either explicitly for reproducibility).",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--segarecomp",
                        help="segarecomp CLI (default: build/dev/apps/segarecomp/segarecomp[.exe])")
    parser.add_argument("--cc", help="strict-C11 C compiler (default: $CC, else cc/clang/gcc on PATH)")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--entry", type=hex_address)
    parser.add_argument("--mapping-base", type=hex_mapping_base)
    parser.add_argument("--compile-jobs", type=int, default=None,
                        help="max concurrent generated-TU compiles (default min(4, CPUs); env SEGARECOMP_COMPILE_JOBS)")
    parser.add_argument("--object-cache-dir",
                        help="opt-in content-addressed cache of compiled generated-TU objects for faster "
                             "iterative rebuilds (env SEGARECOMP_OBJECT_CACHE_DIR; LRU-bounded by "
                             "SEGARECOMP_OBJECT_CACHE_MAX_BYTES, default 4 GiB). Local/ignored only.")
    parser.add_argument("--out-dir", default="build/genesis-startup-bridge")
    parser.add_argument("--mode", required=True, choices=("synthetic", "commercial"))
    parser.add_argument("--expect-sha256")
    parser.add_argument("--full-report-path")
    parser.add_argument("--compare-runs", action="store_true")
    parser.add_argument("--diagnose-frontier", action="store_true",
                        help="emit bounded commercial-derived attribution to stderr for this private session")
    parser.add_argument("--divergence-report",
                        help="with --diagnose-frontier: fold a first-divergence report (JSON from "
                             "tools/m68k_first_divergence.py or tools/genesis_device_divergence.py) into one "
                             "combined ephemeral diagnosis plus a durable-safe class projection")
    parser.add_argument("--build-profile", choices=("auto", "debug", "optimized", "quick"), default="auto",
                        help="generated-program host profile: debug=-O0 -g (low-level debugging), "
                             "optimized=-O2, quick=-O0. auto: optimized for --viewer (long-running), "
                             "quick otherwise (270 MB generated C: -O2 compile dominates one-shot runs)")
    parser.add_argument("--completion-rts", type=hex_address)
    parser.add_argument("--completion-sentinel", type=hex_address)
    # SEG-007-T164 / ADR-0023: the required, explicit, per-invocation opt-in
    # for the external logical-table-descriptor hints interchange file.
    # Forwarded verbatim to the emitter -- this driver never inspects, edits,
    # or trusts the hints content itself; that validation is entirely
    # `parse_genesis_external_hints`'s own responsibility.
    parser.add_argument("--external-hints")
    parser.add_argument("--immutable-rom-aot", action="store_true",
                        help="opt in to complete aligned immutable-ROM AOT enumeration")
    parser.add_argument("--provenance-diagnostics", action="store_true",
                        help="SEG-020-T002: opt in to the generated provenance lookup; the table is "
                             "extracted to <out-dir>/provenance-diagnostics.c (ephemeral, not for commit)")
    # ADR-0013 Decision §7c: optional generic checkpoint/resume path. Only
    # meaningful for the multi-round Phase-B expansion loop (commercial mode
    # with --diagnose-frontier); every other caller shape is unaffected.
    parser.add_argument("--checkpoint")
    # SEG-007-T179 / ADR-0025: the canonical offline-analysis one-shot assisted
    # route. Exactly one generation + one compile + one run; the offline
    # `--external-hints` code-entry inventory is the sole non-reset seed source;
    # 0 runtime-confirmed seeds; no Phase-B expansion loop, no checkpoint, no
    # runtime-confirmed promotion. Reaching unemitted code is reported once as
    # `offline_inventory_incomplete`, never another round.
    parser.add_argument("--one-shot", action="store_true")
    parser.add_argument("--rom-sha256")
    # SEG-007-T252 / ADR-0040: the runner-owned finite dispatch allowance.
    # Omitted means the generated binary's own compiled-in default (128).
    parser.add_argument("--instruction-budget", type=instruction_budget_value)
    # SEG-007-T254: optional interactive SDL3 viewer over the same generated program.
    parser.add_argument("--viewer", action="store_true",
                        help="run the generated program through the SDL3 viewer (requires SDL3)")
    parser.add_argument("--viewer-unthrottled", action="store_true",
                        help="viewer presentation policy: do not sleep between frames")
    parser.add_argument("--viewer-slice", type=instruction_budget_value,
                        help="guest dispatches per host viewer slice (> 0)")
    args = parser.parse_args()
    global _compile_jobs_override, _object_cache_dir_override
    _compile_jobs_override = args.compile_jobs
    if args.object_cache_dir:
        _object_cache_dir_override = pathlib.Path(args.object_cache_dir).resolve()
    if (args.viewer_unthrottled or args.viewer_slice is not None) and not args.viewer:
        sys.stderr.write("--viewer-unthrottled/--viewer-slice require --viewer\n")
        return 8
    if args.viewer and (args.compare_runs or args.full_report_path or args.checkpoint):
        sys.stderr.write("--viewer is incompatible with --compare-runs/--full-report-path/--checkpoint\n")
        return 8
    if (bool(args.completion_rts) != bool(args.completion_sentinel) or
            (args.mapping_base is not None and args.entry is None) or
            (args.full_report_path and args.compare_runs) or
            (args.diagnose_frontier and (args.compare_runs or args.full_report_path)) or
            (args.divergence_report and not args.diagnose_frontier) or
            (args.checkpoint and not (args.mode == "commercial" and args.diagnose_frontier)) or
            # SEG-007-T179 / ADR-0025: --one-shot is the canonical assisted
            # route only; it hard-rejects any expansion/checkpoint/seed input
            # and requires the offline inventory (`--external-hints`).
            (args.one_shot and (args.checkpoint or args.compare_runs or args.full_report_path or
                                 not (args.external_hints or args.immutable_rom_aot) or
                                 args.mode != "commercial" or
                                not args.diagnose_frontier)) or
            (args.mode == "commercial" and
             (args.entry or args.mapping_base or args.completion_rts or
              args.completion_sentinel or args.full_report_path))):
        return 8
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = pathlib.Path(args.segarecomp).resolve() if args.segarecomp else default_segarecomp(root)
    if not args.segarecomp and not binary.exists():
        sys.stderr.write(f"segarecomp binary not found at {binary}; build it first "
                         "(`cmake --workflow --preset dev-build`) or pass --segarecomp\n")
        return 2
    cc_text = select_compiler(args.cc, os.environ.get("CC"))
    if not cc_text or not pathlib.Path(cc_text).exists():
        sys.stderr.write("no C compiler found: install one (cc/clang/gcc) or pass --cc / set CC\n")
        return 2
    compiler = pathlib.Path(cc_text).resolve()
    rom = pathlib.Path(args.rom).resolve()
    out_dir = (pathlib.Path(args.out_dir) if pathlib.Path(args.out_dir).is_absolute()
               else root / args.out_dir).resolve()
    if not ignored(out_dir, root):
        return 6
    digest = hashlib.sha256(rom.read_bytes()).hexdigest()
    if args.expect_sha256 and args.expect_sha256 != digest:
        return 4
    if args.rom_sha256 and args.rom_sha256 != digest:
        return 4
    emitter_command = [*process_tree.script_argv(binary), "emit-general-startup-bridge-c", "--rom", str(rom)]
    if args.entry:
        # Existing project-authored synthetic fixtures are image slices.  Keep
        # their established shorthand while forwarding an explicit map base;
        # callers that need an independent mapping can provide --mapping-base.
        emitter_command += ["--entry", args.entry, "--mapping-base", args.mapping_base or args.entry]
    else:
        emitter_command += ["--reset-entry"]
    emitter_command += ["--rom-sha256", digest]
    if args.completion_rts:
        emitter_command += ["--synthetic-completion-rts", args.completion_rts,
                            "--synthetic-completion-sentinel", args.completion_sentinel]
    if args.external_hints:
        emitter_command += ["--external-hints", args.external_hints]
    if args.immutable_rom_aot:
        emitter_command += ["--immutable-rom-aot"]
    if args.provenance_diagnostics:
        emitter_command += ["--provenance-diagnostics"]
    viewer_sdl3 = None
    if args.viewer:
        # Fail clearly before any generation/guest execution; never fall back to headless.
        viewer_sdl3 = find_sdl3()
        if viewer_sdl3 is None:
            sys.stderr.write("SDL3 viewer support is unavailable: SDL3 headers/library not found "
                             "(install SDL3 or set SDL3_PREFIX); not falling back to headless\n")
            return 9
    out_dir.mkdir(parents=True, exist_ok=True)
    def single_cycle_generate_and_compile() -> tuple[int, bytes | None, pathlib.Path | None]:
        return generate_and_compile(emitter_command, compiler, root, out_dir, resolve_profile(args),
                                    viewer_sdl3)
    if args.viewer:
        # One generation + one compile + one interactive run of the same generated program.
        status, _, executable = single_cycle_generate_and_compile()
        if status:
            return status
        assert executable is not None
        return run_viewer(executable, root,
                          args.instruction_budget if args.instruction_budget is not None
                          else GENESIS_CANONICAL_RUNNER_DISPATCH_ALLOWANCE,
                          args.viewer_unthrottled, args.viewer_slice)
    full_path = ((pathlib.Path(args.full_report_path) if pathlib.Path(args.full_report_path).is_absolute()
                  else root / args.full_report_path).resolve() if args.full_report_path else None)
    # ADR-0013 Decision §7 Phase B: the multi-round build/execute/expand loop
    # is enabled for exactly the authorized pinned-route invocation shape
    # (commercial mode with --diagnose-frontier); every other existing caller
    # shape (synthetic mode, --compare-runs, plain commercial without
    # --diagnose-frontier) keeps today's single generate/compile/run/report
    # cycle byte-identically, so no existing fixture's behavior changes.
    if args.one_shot:
        # SEG-007-T179 / ADR-0025 canonical one-shot assisted route: exactly one
        # generation + one compile + one run. No expansion loop, no checkpoint,
        # no runtime-confirmed seed promotion. `emitter_command` already carries
        # `--reset-entry` and `--external-hints <artifact>`; no `--analysis-seed`
        # is ever appended.
        status, generated_bytes, executable = single_cycle_generate_and_compile()
        if status:
            # Normalized metrics count ACTUAL invocations, not intended pipeline
            # stages. `status == 1` is an emitter/static-translation rejection
            # that returns before any C is written and before the C compiler is
            # invoked, so `compile_attempts = 0`. `status == 2` is the C compiler
            # itself failing after exactly one successful generation, so
            # `compile_attempts = 1`.
            sys.stderr.write("ONE_SHOT_SUMMARY " + json.dumps(
                {"runtime_confirmed_seed_count": 0, "generation_attempts": 1,
                 "compile_attempts": 0 if status == 1 else 1,
                 "driver_result": "build_time_translation_rejected" if status == 1
                 else "strict_c11_compile_failed"}, separators=(",", ":")) + "\n")
            return status
        assert executable is not None
        if args.provenance_diagnostics:
            marker = b"\n/* SEG-020-T002 provenance diagnostics"
            import mmap
            _shard_main = out_dir / "generated" / "bridge_generated_main.c"
            with (_shard_main if _shard_main.is_file() else out_dir / "bridge.generated.c").open("rb") as generated_file, \
                    mmap.mmap(generated_file.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
                start = mapped.find(marker)
                if start >= 0:
                    (out_dir / "provenance-diagnostics.c").write_bytes(mapped[start:])
                    sys.stderr.write("PROVENANCE_DIAGNOSTICS " + str(out_dir / "provenance-diagnostics.c") + "\n")
        # SEG-007-T252 / ADR-0040 correction: the canonical/headless one-shot
        # route never silently falls through to the generated binary's own
        # lower-level zero-argument default (128) when the operator omits
        # `--instruction-budget`; it uses the named canonical allowance
        # instead. The generated binary's own compiled-in 128 default remains
        # unchanged for callers that invoke it directly without going through
        # this CLI.
        one_shot_instruction_budget = (args.instruction_budget if args.instruction_budget is not None
                                       else GENESIS_CANONICAL_RUNNER_DISPATCH_ALLOWANCE)
        run_status, report, full_bytes, message, ephemeral_bytes = run_bridge(
            executable, root, full_pipe=True, instruction_budget=one_shot_instruction_budget,
            ephemeral_pipe=True)
        if run_status:
            if message:
                sys.stderr.write(message)
            # One generation and one compile both completed before the run.
            sys.stderr.write("ONE_SHOT_SUMMARY " + json.dumps(
                {"runtime_confirmed_seed_count": 0, "generation_attempts": 1, "compile_attempts": 1,
                 "driver_result": "run_failed"}, separators=(",", ":")) + "\n")
            return run_status
        if report_sha_status(report, digest) or not valid_sanitized(report, digest):
            return 5
        full = parse_canonical_full(full_bytes if full_bytes is not None else b"")
        if full is None or report_sha_status(full, digest) or not valid_full(full, report, digest):
            return 5
        completed = report["result"] == "completed"
        # SEG-007-T252 / ADR-0040: runner_resource_limit is its own normalized
        # frontier class, never conflated with a guest `stop_class` (which is
        # null here) nor silently reported as "unknown".
        frontier_class = ("completed" if completed
                          else "runner_resource_limit" if report["result"] == "runner_resource_limit"
                          else report.get("stop_class", "unknown"))
        # SEG-007-T252 correction: a runner-resource-limit frontier is host
        # invocation policy, not a guest/static inventory deficiency -- it
        # must never normalize to "offline_inventory_incomplete", which is
        # reserved for an actual guest/static inventory stop.
        if completed:
            driver_result = "completed"
        elif report["result"] == "runner_resource_limit":
            driver_result = "runner_resource_limit"
        else:
            driver_result = "offline_inventory_incomplete"
        stitch_metrics = offline_inventory_stitch_metrics(executable.parent)
        recent_pc_history = parse_ephemeral_pc_history(ephemeral_bytes if ephemeral_bytes is not None else b"")
        ephemeral = with_execution_history(ephemeral_frontier(full, executable, recent_pc_history), ephemeral_bytes)
        if stitch_metrics:
            ephemeral["offline_inventory_stitch_metrics"] = stitch_metrics
        sys.stderr.write("EPHEMERAL_FRONTIER " +
                         json.dumps(ephemeral, separators=(",", ":")) + "\n")
        write_combined_diagnosis(args.divergence_report, report, ephemeral)
        one_shot_summary = {"runtime_confirmed_seed_count": 0, "generation_attempts": 1,
                            "compile_attempts": 1,
                            "driver_result": driver_result, "frontier_class": frontier_class}
        if stitch_metrics:
            one_shot_summary["offline_inventory_stitch_metrics"] = stitch_metrics
        sys.stderr.write("ONE_SHOT_SUMMARY " + json.dumps(one_shot_summary, separators=(",", ":")) + "\n")
        print(json.dumps(report, separators=(",", ":")))
        return 0
    if args.mode == "commercial" and args.diagnose_frontier:
        checkpoint_path = ((pathlib.Path(args.checkpoint) if pathlib.Path(args.checkpoint).is_absolute()
                            else root / args.checkpoint).resolve() if args.checkpoint else None)
        initial_seeds = (load_checkpoint(checkpoint_path, digest, args.external_hints)
                         if checkpoint_path is not None else [])
        # ADR-0013 Decision §7c: an operational Phase-B batch boundary
        # (`PHASE_B_BATCH_COMPLETE`) is resumable driver state, not a stopping
        # point requiring a human/task/refinement intervention. When a
        # `--checkpoint` path is supplied, automatically re-invoke
        # `run_expansion_loop` with the freshly-saved checkpoint's seeds as
        # the next `initial_seeds`, repeating until either a genuine terminal
        # (anything other than `PHASE_B_BATCH_COMPLETE`) is reached or
        # `MAX_OUTER_RESUME_BATCHES` is hit (a defensive infinite-loop safety
        # net only -- see that constant's own doc comment). Without
        # `--checkpoint` this loop runs exactly once, byte-identical to the
        # prior single-batch behavior every existing caller/fixture relies on.
        outer_batches = 0
        status = report = full_bytes = executable = summary = message = final_seeds = None
        while True:
            outer_batches += 1
            status, report, full_bytes, executable, summary, message, final_seeds = run_expansion_loop(
                emitter_command, compiler, root, out_dir, digest, initial_seeds, args.instruction_budget)
            # ADR-0013 Decision §7c: always write updated checkpoint state
            # back after each batch, regardless of outcome -- the confirmed-
            # root set is persistent program knowledge even when a batch ends
            # in a build/run failure partway through a later round.
            if checkpoint_path is not None:
                save_checkpoint(checkpoint_path, digest, args.external_hints, final_seeds)
            if status:
                break
            if (checkpoint_path is None or summary.get("driver_result") != PHASE_B_BATCH_COMPLETE or
                    outer_batches >= MAX_OUTER_RESUME_BATCHES):
                break
            initial_seeds = final_seeds
        if checkpoint_path is not None:
            # Only surface the outer-batch count when checkpoint/resume is
            # actually in play; without --checkpoint this loop always runs
            # exactly once and the summary shape stays byte-identical to the
            # prior single-batch behavior every existing caller/fixture
            # relies on.
            summary = dict(summary)
            summary["outer_batches"] = outer_batches
        if status:
            if message: sys.stderr.write(message)
            # The loop's own normalized summary is bounded evidence even on a
            # terminal failure exit (e.g. a later round's build-time
            # translation rejection) -- never a raw address on its own.
            sys.stderr.write("EXPANSION_SUMMARY " + json.dumps(summary, separators=(",", ":")) + "\n")
            return status
        assert report is not None and executable is not None
        full = parse_canonical_full(full_bytes if full_bytes is not None else b"")
        if full is None or report_sha_status(full, digest) or not valid_full(full, report, digest):
            return 5
        expansion_frontier = ephemeral_frontier(full, executable)
        expansion_stitch_metrics = offline_inventory_stitch_metrics(executable.parent)
        if expansion_stitch_metrics:
            expansion_frontier["offline_inventory_stitch_metrics"] = expansion_stitch_metrics
        sys.stderr.write(
            "EPHEMERAL_FRONTIER " +
            json.dumps(expansion_frontier, separators=(",", ":")) +
            "\n")
        write_combined_diagnosis(args.divergence_report, report, expansion_frontier)
        # The loop's own normalized summary (round count, terminal driver/stop
        # class, final seed-set size): never a raw address, never printed to
        # stdout, never durable evidence on its own.
        sys.stderr.write("EXPANSION_SUMMARY " + json.dumps(summary, separators=(",", ":")) + "\n")
        print(json.dumps(report, separators=(",", ":")))
        return 0
    if args.compare_runs:
        first_status, _, executable = single_cycle_generate_and_compile()
        if first_status:
            return first_status
        assert executable is not None
        first_status, first, first_full, message, _ = run_bridge(executable, root, full_pipe=True,
                                                                  instruction_budget=args.instruction_budget)
        if first_status:
            sys.stderr.write(message)
            return first_status
        second_status, second, second_full, message, _ = run_bridge(executable, root, full_pipe=True,
                                                                     instruction_budget=args.instruction_budget)
        if second_status:
            sys.stderr.write(message)
            return second_status
        first_sha_status = report_sha_status(first, digest)
        second_sha_status = report_sha_status(second, digest)
        if first_sha_status or second_sha_status:
            return 4 if first_sha_status == 4 or second_sha_status == 4 else 5
        if not valid_sanitized(first, digest) or not valid_sanitized(second, digest):
            return 5
        first_full_report = parse_canonical_full(first_full if first_full is not None else b"")
        second_full_report = parse_canonical_full(second_full if second_full is not None else b"")
        if first_full_report is None or second_full_report is None: return 5
        first_full_sha_status = report_sha_status(first_full_report, digest)
        second_full_sha_status = report_sha_status(second_full_report, digest)
        if first_full_sha_status or second_full_sha_status:
            return 4 if first_full_sha_status == 4 or second_full_sha_status == 4 else 5
        if not valid_full(first_full_report, first, digest) or not valid_full(second_full_report, second, digest):
            return 5
        report = dict(first)
        report["reports_match"] = first_full == second_full
        print(json.dumps(report, separators=(",", ":")))
        return 0 if report["reports_match"] else 7
    status, _, executable = single_cycle_generate_and_compile()
    if status:
        return status
    assert executable is not None
    status, report, full_bytes, message, ephemeral_bytes = run_bridge(
        executable, root, full_path, full_pipe=args.diagnose_frontier,
        instruction_budget=args.instruction_budget, ephemeral_pipe=args.diagnose_frontier)
    if status:
        sys.stderr.write(message)
        return status
    sha_status = report_sha_status(report, digest)
    if sha_status:
        return sha_status
    if not valid_sanitized(report, digest):
        return 5
    if args.diagnose_frontier:
        full = parse_canonical_full(full_bytes if full_bytes is not None else b"")
        if full is None or report_sha_status(full, digest) or not valid_full(full, report, digest):
            return 5
        recent_pc_history = parse_ephemeral_pc_history(ephemeral_bytes if ephemeral_bytes is not None else b"")
        single_frontier = with_execution_history(ephemeral_frontier(full, executable, recent_pc_history), ephemeral_bytes)
        sys.stderr.write("EPHEMERAL_FRONTIER " + json.dumps(single_frontier, separators=(",", ":")) + "\n")
        write_combined_diagnosis(args.divergence_report, report, single_frontier)
    if full_path is not None:
        try: full = parse_canonical_full(full_path.read_bytes())
        except OSError: return 5
        if full is None: return 5
        sha_status = report_sha_status(full, digest)
        if sha_status:
            return sha_status
        if not valid_full(full, report, digest):
            return 5
    print(json.dumps(report, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
