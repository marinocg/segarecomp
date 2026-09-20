#!/usr/bin/env python3
"""SEG-007-T007: local Sonic startup capability inventory scanner.

A project-owned, opt-in, local-only development tool. Given a hash-pinned local Sonic ROM and a
pinned local Musashi checkout/adapter, it performs one deterministic, explicitly bounded startup
run under the reference Musashi interpreter, then classifies every traced instruction/access
through segarecomp's own real shared CPU decode pipeline and memory/address-space mapping
policy (never a scanner-private model), and writes a normalized, privacy-safe capability
inventory to a gitignored local cache path.

The adapter is a dumb, bounded, single-instruction-at-a-time Musashi stepper with no semantic
authority: it never decides ``hardware_frontier`` (or any other stop reason) itself. This module
is the sole authority for every bounded-execution stop decision, made fresh on every run against
the real production ``probe-genesis-startup-mapping``/``probe-genesis-startup-decode`` CLI
probes, via a synchronous, per-instruction, in-memory streaming protocol over the adapter's
stdin/stdout pipes (``run_scan``). No raw trace is ever written to any file (temporary or
otherwise); a single instruction's raw parsed JSON line exists only as a local variable for the
duration of one loop iteration in ``run_scan`` and is discarded (garbage-collected) before the
next line is even read.

See ``docs/testing/sonic-startup-inventory.md`` for the full workflow, adapter bootstrap
instructions, and privacy rules; sanitized findings feed later backlog refinement.

This module never prints, logs, or writes raw ROM-derived content (ROM bytes, opcode/extension
words, disassembly, or the local ROM path) anywhere; every value that reaches stdout or the cache
file has already passed through normalization (``stage1_classifier.classify`` and the
``probe-genesis-startup-decode``/``probe-genesis-startup-mapping`` CLI probes).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
from typing import Optional

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

from tools import process_tree  # noqa: E402
from tools.inventory import stage1_classifier  # noqa: E402

# Pinned by SEG-006-T001 / SEG-007-T004; see
# docs/references/sonic-first-unsupported-inspection-contract.md.
PINNED_ROM_SHA256 = "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6"
# Pinned by the musashi-oracle-validation skill.
PINNED_MUSASHI_REVISION = "313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"
# Expected adapter executable name inside $SEGARECOMP_MUSASHI_TOOL_DIR; see
# docs/testing/sonic-startup-inventory.md for its bootstrap instructions and source.
ADAPTER_NAME = "sonic-startup-musashi-scan-oracle"
CACHE_RELATIVE_PATH = pathlib.Path(".cache") / "sonic-startup-inventory.json"

# Literal, documented bounded-execution limits. The scanner never runs unbounded. These are
# decided entirely by this module (never by the adapter) against the real, live production
# probes on every run.
MAX_EXECUTED_INSTRUCTIONS = 500
MAX_UNIQUE_VISITED_PCS = 500
MAX_CONTROL_FLOW_DEPTH = 32
# An adapter is untrusted even after it has sent a protocol line.  This is deliberately a literal
# bound rather than a caller option: shutdown must not turn a malformed line into an unbounded
# wait for an adapter which leaves one or more pipes open.
ADAPTER_SHUTDOWN_TIMEOUT_SECONDS = 1
# Waiting for the next adapter record is also lifecycle waiting.  A started adapter which keeps
# stdout open but never produces a complete record must not make the scan wait indefinitely.
ADAPTER_PROTOCOL_LINE_TIMEOUT_SECONDS = 1
# A newline-free hostile stream must not turn the per-line buffer into unbounded retained memory.
MAX_PROTOCOL_LINE_BYTES = 4096
# Production probes are supplied executables, so their lifecycle and output are bounded just as the
# adapter protocol is.  Probe JSON is deliberately small; retain no more than this fixed response.
PROBE_TIMEOUT_SECONDS = 1
MAX_PROBE_STDOUT_BYTES = 4096

# Adapter streaming protocol (all values are untrusted until validated in ``run_scan``): each
# instruction is exactly {ordinal, pc, primary, extension, length, accesses}; each access is
# exactly {ordinal, kind, address, byteWidth}. ``primary`` is ``0x`` plus four hexadecimal
# digits, ``extension`` is null or ``0x`` plus eight hexadecimal digits, and program/data
# addresses are ``0x`` plus eight hexadecimal digits. Ordinals are unsigned 64-bit values and
# lengths are unsigned 32-bit values. Access kinds are the closed read/write enum and widths are
# the MC68000 byte/word/long widths. These limits ensure no adapter-provided value is masked or
# otherwise silently reinterpreted before a production probe or normalized cache entry sees it.
MAX_PROTOCOL_ORDINAL = 0xFFFFFFFFFFFFFFFF
MAX_PROTOCOL_LENGTH = 0xFFFFFFFF
VALID_ACCESS_KINDS = frozenset(("read", "write"))
VALID_ACCESS_BYTE_WIDTHS = frozenset((1, 2, 4))
# Validate this before classifying the instruction or invoking either production probe.  It bounds
# both normalized aggregation and the number of mapping probes attributable to one wire record.
MAX_ACCESS_RECORDS_PER_INSTRUCTION = 64

EXIT_SUCCESS = 0
EXIT_ROM_UNAVAILABLE = 10
EXIT_MUSASHI_UNAVAILABLE = 20
EXIT_USAGE = 2
# A completed scan whose normalized inventory contains at least one "semantic_classifier_gap"
# instruction entry is not safe to seed a compatibility batch from: the classifier itself needs
# extending for that observed form first. The scan itself still succeeds as a diagnostic run (the
# cache file is written and the sanitized summary is printed), but main() fails closed for
# backlog-generation purposes by returning this exit code instead of EXIT_SUCCESS.
EXIT_SEMANTIC_GAP = 30
# The adapter/protocol failed to complete through a documented stop condition: stdout hit EOF
# before this module ever decided one of the four documented stop reasons and sent "stop", a line
# could not be parsed as the expected wire protocol, a write to the adapter's stdin hit a broken
# pipe, or the adapter's exit code was nonzero even after a clean "stop" reply. None of these are
# a successful scan; no cache file is written by a run that exits with this code.
EXIT_ADAPTER_FAILURE = 40


class AdapterFailure(Exception):
    """Raised by run_scan on any adapter lifecycle/protocol failure.

    ``reason`` is a short, already-sanitized diagnostic tag (never raw adapter stderr or any
    ROM-derived content) -- e.g. "unexpected_eof_before_stop", "nonzero_exit",
    "malformed_protocol", "broken_pipe". ``adapter_stderr`` is deliberately always empty: reading
    an untrusted stderr pipe can itself block forever when a descendant retains the pipe, and raw
    adapter output must not be retained or surfaced.
    """

    def __init__(self, reason: str, adapter_stderr: str = "") -> None:
        super().__init__(reason)
        self.reason = reason
        self.adapter_stderr = adapter_stderr

HELP_TEXT = f"""Local Sonic startup capability inventory scanner (SEG-007-T007).

Usage:
  python3 tools/sonic_startup_inventory.py --help
  python3 tools/sonic_startup_inventory.py --executable <path-to-built-segarecomp-binary> --scan

Local input gate (ROM):
  Set SEGARECOMP_SONIC_ROM to an absolute path naming a local, legally held Genesis Sonic the
  Hedgehog image. The file must exist and its SHA-256 digest must equal the pinned digest:
    {PINNED_ROM_SHA256}
  A missing variable, missing file, or digest mismatch is a graceful "unavailable" outcome
  (never a crash); the tool never prints the configured path back to the user.

Pinned oracle gate (Musashi):
  Set SEGARECOMP_MUSASHI_TOOL_DIR to the absolute ignored local tool directory containing:
    - a "musashi" checkout at the pinned revision {PINNED_MUSASHI_REVISION}
    - the built adapter executable at $SEGARECOMP_MUSASHI_TOOL_DIR/{ADAPTER_NAME}
  See docs/testing/sonic-startup-inventory.md for the adapter's full source and bootstrap
  commands (mirroring the musashi-oracle-validation skill's Docker+gcc:13 convention). A missing
  variable, missing/mismatched checkout, or missing adapter is a graceful "unavailable" outcome.

Bounded execution (every scan is bounded; the first triggered stop condition ends the run, in
this exact precedence). The adapter is a dumb, bounded, single-instruction-at-a-time stepper: it
never decides any of these itself. This module decides every stop, on every run, against the
real, live production probes, streamed one fully-executed instruction at a time over the
adapter's stdin/stdout pipes:
  1. first observed memory/device access classified "hardware_frontier" by the real shared
      production mapping policy (segarecomp probe-genesis-startup-mapping), width-aware: an
      access is accepted only when its whole [address, address+width) range stays inside a mapped
      window; a partially-out-of-range/crossing access is itself a hardware_frontier stop.
  2. MAX_EXECUTED_INSTRUCTIONS = {MAX_EXECUTED_INSTRUCTIONS}   (total instructions executed)
  3. MAX_UNIQUE_VISITED_PCS   = {MAX_UNIQUE_VISITED_PCS}   (unique program counters visited)
  4. MAX_CONTROL_FLOW_DEPTH   = {MAX_CONTROL_FLOW_DEPTH}    (taken branches/calls)
  Adapter/probe safety limits: adapter records wait at most {ADAPTER_PROTOCOL_LINE_TIMEOUT_SECONDS}s,
  adapter lines retain at most {MAX_PROTOCOL_LINE_BYTES} bytes, and each production probe waits at
  most {PROBE_TIMEOUT_SECONDS}s and retains at most {MAX_PROBE_STDOUT_BYTES} stdout bytes.

Output:
  The full normalized machine-readable inventory is written only to the gitignored local cache
  path: {CACHE_RELATIVE_PATH}
  Only a bounded, already-sanitized human summary (counts per support/category value and the
  stop reason) is printed to stdout. No raw ROM byte, opcode/extension word, disassembly text,
  or local ROM path is ever printed, logged, or written anywhere by this tool, and the raw
  per-instruction trace is never written to any file (temporary or otherwise) at any point.

Exit codes:
  {EXIT_SUCCESS}  successful bounded scan (including --help)
  {EXIT_USAGE}  usage error (bad arguments)
  {EXIT_ROM_UNAVAILABLE} ROM unavailable (unset/missing/hash-mismatched SEGARECOMP_SONIC_ROM)
  {EXIT_MUSASHI_UNAVAILABLE} Musashi pin unavailable (unset/missing/mismatched SEGARECOMP_MUSASHI_TOOL_DIR)
  {EXIT_SEMANTIC_GAP} scan completed but at least one observed capability could not be classified
      precisely enough to seed a compatibility batch (a "semantic_classifier_gap" instruction
      entry was observed); the classifier itself needs extending for that form before backlog
      refinement can safely consume this run's output. The cache file and summary are still
      written/printed -- this is a diagnostic success with a backlog-generation-unsafe result.
  {EXIT_ADAPTER_FAILURE} adapter/protocol failure -- the scan did not complete through a documented
      stop condition; no cache file was written by this run. A successful scan requires this
      module to have explicitly decided one of the four documented stop conditions, sent "stop",
      and the adapter process to then exit 0; any other outcome (unexpected EOF before "stop" was
      sent, a malformed/unparseable protocol line, a broken stdin pipe, or a nonzero adapter exit
      code even after a clean "stop" reply) is this failure. The previous run's cache file (if
      any) is left untouched -- never overwritten, never deleted -- by a run that fails this way.
"""


def check_rom_gate() -> "tuple[Optional[pathlib.Path], Optional[str]]":
    """Gate 1. Returns (rom_path, None) on success, or (None, message) on failure.

    Never includes the configured path in the returned message, per privacy policy.
    """
    env_path = os.environ.get("SEGARECOMP_SONIC_ROM")
    if not env_path:
        return None, "SEGARECOMP_SONIC_ROM is unset"
    rom_path = pathlib.Path(env_path)
    if not rom_path.is_file():
        return None, "SEGARECOMP_SONIC_ROM does not name a readable file"
    try:
        digest = hashlib.sha256(rom_path.read_bytes()).hexdigest()
    except OSError:
        return None, "SEGARECOMP_SONIC_ROM does not name a readable file"
    if digest != PINNED_ROM_SHA256:
        return None, "local ROM does not match the pinned SHA-256 digest"
    return rom_path, None


def check_musashi_gate() -> "tuple[Optional[pathlib.Path], Optional[str]]":
    """Gate 2. Returns (adapter_path, None) on success, or (None, message) on failure."""
    tool_dir = os.environ.get("SEGARECOMP_MUSASHI_TOOL_DIR")
    if not tool_dir:
        return None, "SEGARECOMP_MUSASHI_TOOL_DIR is unset"
    tool_root = pathlib.Path(tool_dir)
    musashi_checkout = tool_root / "musashi"
    if not musashi_checkout.is_dir():
        return None, "local musashi checkout is missing"
    try:
        result = subprocess.run(
            ["git", "-C", str(musashi_checkout), "rev-parse", "HEAD"],
            text=True, capture_output=True, check=False,
        )
    except OSError:
        return None, "unable to inspect the local musashi checkout"
    revision = result.stdout.strip()
    if result.returncode != 0 or revision != PINNED_MUSASHI_REVISION:
        return None, "local musashi checkout is not at the pinned revision"
    adapter_path = tool_root / ADAPTER_NAME
    if not adapter_path.is_file() or not os.access(adapter_path, os.X_OK):
        return None, "pinned musashi adapter executable is missing"
    return adapter_path, None


def _parse_protocol_hex(value: object, digits: int) -> str:
    """Validate and normalize one fixed-width adapter hexadecimal value."""
    if not isinstance(value, str) or len(value) != digits + 2 or not value.startswith("0x"):
        raise TypeError("protocol hexadecimal value has an invalid shape")
    hexadecimal_digits = value[2:]
    if any(character not in "0123456789abcdefABCDEF" for character in hexadecimal_digits):
        raise ValueError("protocol hexadecimal value has invalid digits")
    return hexadecimal_digits.upper()


def _parse_protocol_uint(value: object, maximum: int) -> int:
    """Validate one bounded, non-negative adapter integer without coercion."""
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= maximum:
        raise TypeError("protocol integer is out of range")
    return value


def _probe_decode(executable: str, primary_hex: str, extension_hex: str) -> dict:
    result = _run_bounded_probe(
        [executable, "probe-genesis-startup-decode", primary_hex, extension_hex],
    )
    if not isinstance(result, dict) or not isinstance(result.get("support"), str):
        raise AdapterFailure("probe_failure")
    return result


def _probe_mapping(executable: str, address_hex: str, width: int, length_hex: str) -> dict:
    result = _run_bounded_probe(
        [executable, "probe-genesis-startup-mapping", address_hex, str(width), length_hex],
    )
    if not isinstance(result, dict) or not isinstance(result.get("category"), str):
        raise AdapterFailure("probe_failure")
    return result


def _run_bounded_probe(command: list[str]) -> object:
    """Run one production probe with bounded wall time and stdout retention.

    Stderr is redirected rather than captured, and stdout is retained only up to the documented
    protocol-sized limit.  All failures are deliberately reduced to sanitized AdapterFailure tags.
    """
    try:
        process = subprocess.Popen(
            [*process_tree.script_argv(command[0]), *command[1:]], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            **process_tree.popen_kwargs(),
        )
    except OSError as error:
        raise AdapterFailure("probe_failure") from error
    process_tree.attach(process)
    assert process.stdout is not None
    output = b""
    deadline = time.monotonic() + PROBE_TIMEOUT_SECONDS
    failure: Optional[str] = None
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                failure = "probe_timeout"
                break
            chunk = process_tree.read_chunk(process.stdout, MAX_PROBE_STDOUT_BYTES + 1, remaining)
            if chunk is None:
                failure = "probe_timeout"
                break
            if not chunk:
                break
            if len(output) + len(chunk) > MAX_PROBE_STDOUT_BYTES:
                failure = "probe_output_limit"
                break
            output += chunk
        if failure is None and process.wait(timeout=max(0, deadline - time.monotonic())) != 0:
            failure = "probe_failure"
    except (OSError, subprocess.TimeoutExpired):
        failure = "probe_timeout"
    finally:
        # The probe owns a fresh session.  Signal the whole group even if its immediate parent
        # already exited: a descendant may otherwise retain a pipe or CPU after a nominal exit.
        process_tree.terminate(process, force=False)
        if process.poll() is None:
            try:
                process.wait(timeout=ADAPTER_SHUTDOWN_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process_tree.terminate(process, force=True)
                try:
                    process.wait(timeout=ADAPTER_SHUTDOWN_TIMEOUT_SECONDS)
                except subprocess.TimeoutExpired:
                    pass
        process_tree.forget(process.stdout)
        try:
            process.stdout.close()
        except OSError:
            pass
        process_tree.release(process)
    if failure is not None:
        raise AdapterFailure(failure)
    try:
        return json.loads(output.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AdapterFailure("probe_failure") from error


def _bounded_adapter_shutdown(process: subprocess.Popen[str]) -> None:
    """Close adapter pipes and terminate/reap their process group without unbounded waits.

    The process may be malicious or malformed-protocol handling may happen while it is still
    running.  The adapter owns a fresh session, so every cleanup path signals its full process
    group: an adapter parent must not be able to leave a pipe-holding descendant behind. Give the
    immediate adapter normal completion and termination one fixed interval each, then always kill
    any remaining group member. The final timeout is intentionally not followed by another wait:
    that would violate the scan bound.
    """
    try:
        process.stdin.close()
    except (BrokenPipeError, OSError, ValueError):
        pass

    if process.poll() is None:
        try:
            process.wait(timeout=ADAPTER_SHUTDOWN_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            pass

    # Do this even when the immediate adapter already exited: a descendant may retain the group
    # and any inherited pipes. Process-group signalling mirrors bounded probe cleanup and avoids
    # relying on the adapter parent to propagate termination.
    process_tree.terminate(process, force=False)

    if process.poll() is None:
        try:
            process.wait(timeout=ADAPTER_SHUTDOWN_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            pass

    # Always kill the group after its bounded SIGTERM grace period, including when the adapter
    # parent has already been reaped. A SIGTERM-ignoring descendant must not survive cleanup.
    process_tree.terminate(process, force=True)

    if process.poll() is None:
        try:
            process.wait(timeout=ADAPTER_SHUTDOWN_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            # A kernel-uninterruptible immediate process cannot be reaped here. Do not replace
            # the bounded contract with an unbounded wait; it will be reaped once killable.
            pass

    # Never call stderr.read(): a child/descendant can retain that pipe after the adapter exits.
    # Closing our ends releases resources without retaining potentially ROM-derived diagnostics.
    for stream in (process.stdout, process.stderr):
        try:
            if stream is not None:
                process_tree.forget(stream)
                stream.close()
        except OSError:
            pass
    process_tree.release(process)


def _read_protocol_line(stdout: object, buffered: bytes) -> "tuple[Optional[str], bytes, bool]":
    """Read one complete protocol line with a fixed timeout, retaining only partial wire bytes.

    ``TextIOWrapper.readline`` cannot be used here: after an adapter writes a partial line it may
    block indefinitely waiting for the newline.  Read ready bytes directly and apply the timeout
    again until a complete line is available.
    """
    while b"\n" not in buffered:
        chunk = process_tree.read_chunk(stdout, 4096, ADAPTER_PROTOCOL_LINE_TIMEOUT_SECONDS)
        if chunk is None:
            return None, buffered, False
        if not chunk:
            if not buffered:
                return "", buffered, False
            # Let the existing malformed-protocol path handle an unterminated final record.
            return buffered.decode("utf-8", errors="replace"), b"", False
        if len(buffered) + len(chunk) > MAX_PROTOCOL_LINE_BYTES:
            return None, b"", True
        buffered += chunk

    line, buffered = buffered.split(b"\n", 1)
    return line.decode("utf-8", errors="replace"), buffered, False


def decide_stop_reason(hardware_frontier_hit: bool, total_instructions: int,
                       unique_visited_pcs: int, control_flow_depth: int) -> Optional[str]:
    """Apply the scanner's fixed post-instruction stop precedence.

    Counters are supplied after incorporating the just-executed instruction.  Keeping this
    decision small and pure makes the equal 500 instruction/PC bounds independently testable
    without changing either documented limit or the streaming protocol.
    """
    if hardware_frontier_hit:
        return "hardware_frontier"
    if total_instructions >= MAX_EXECUTED_INSTRUCTIONS:
        return "max_executed_instructions"
    if unique_visited_pcs >= MAX_UNIQUE_VISITED_PCS:
        return "max_unique_visited_pcs"
    if control_flow_depth >= MAX_CONTROL_FLOW_DEPTH:
        return "max_control_flow_depth"
    return None


def run_scan(adapter_path: pathlib.Path, rom_path: pathlib.Path, executable: str,
             image_length: int) -> "tuple[dict, str]":
    """Drive the adapter's synchronous, per-instruction, in-memory streaming protocol.

    This function is the sole authority for every bounded-execution stop decision (never the
    adapter): it decides, after each fully-executed instruction the adapter reports, whether to
    reply "continue" (let the adapter run one more instruction) or "stop" (end the run), using
    only real facts it derives itself (instruction/PC/control-flow-depth counters) plus the real
    shared production probes (``probe-genesis-startup-decode``/``probe-genesis-startup-mapping``)
    -- never a scanner-private mapping/support model.

    The adapter's raw per-instruction JSON line is parsed into a local variable (``entry``) that
    is used only for the duration of the current loop iteration and is never accumulated into any
    list, written to any file, printed, or logged; only the normalized aggregation dicts
    (``instructions``/``accesses``) persist across iterations. This is a *stronger* privacy
    property than buffering the whole raw trace in memory: raw content for instruction K is
    garbage-collected before instruction K+1's line is even read.

    Contract: this function either returns a fully successful ``(normalized_inventory,
    adapter_stderr_text)`` tuple, or raises ``AdapterFailure``. There is no third, partial-success
    outcome. A successful scan is only one where this function itself decided one of the four
    documented stop conditions (``hardware_frontier`` / ``max_executed_instructions`` /
    ``max_unique_visited_pcs`` / ``max_control_flow_depth``), sent ``"stop\\n"``, and the adapter
    process then exited with code 0. Every other outcome -- the adapter's stdout hitting EOF
    before this function ever decided a stop condition (covering an early/unexpected exit, a
    crash, and the adapter's own internal hard safety ceiling firing on its own; this function
    cannot distinguish those from each other and does not need to, since they are all "the
    protocol ended before I decided a documented stop condition"), a line that cannot be parsed as
    the expected wire protocol, a broken pipe while writing a reply, or a nonzero adapter exit
    code even after a clean "stop" reply -- raises ``AdapterFailure`` instead. ``adapter_stderr``
    is always empty: stderr is closed rather than read during bounded cleanup, so raw adapter
    output is neither retained nor surfaced.
    """
    process = subprocess.Popen(
        [*process_tree.script_argv(adapter_path), str(rom_path)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, bufsize=1, **process_tree.popen_kwargs(),
    )
    process_tree.attach(process)
    assert process.stdin is not None and process.stdout is not None

    instructions_aggregated: "dict[tuple, dict]" = {}
    instructions_order: list = []
    accesses_aggregated: "dict[tuple, dict]" = {}
    accesses_order: list = []

    total_instructions = 0
    unique_pcs: "set[int]" = set()
    control_flow_depth = 0
    previous_pc: Optional[int] = None
    previous_length: Optional[int] = None
    stop_reason: Optional[str] = None
    stop_sent = False
    failure_reason: Optional[str] = None
    length_hex = f"{image_length & 0xFFFFFFFFFFFFFFFF:016X}"
    protocol_buffer = b""

    try:
        while True:
            line, protocol_buffer, line_too_long = _read_protocol_line(
                process.stdout, protocol_buffer,
            )
            if line_too_long:
                failure_reason = "malformed_protocol"
                break
            if line is None:
                failure_reason = "protocol_timeout"
                break
            if line == "":
                # EOF before this function ever decided a documented stop condition: an
                # early/unexpected adapter exit, a crash, or the adapter's own internal hard
                # safety ceiling firing -- all uniformly a protocol failure, never a success.
                failure_reason = "unexpected_eof_before_stop"
                break
            line = line.strip()
            if not line:
                # The wire protocol has no ignorable records.  In particular, accepting blank
                # lines would let a hostile adapter keep this loop alive without advancing a bound.
                failure_reason = "malformed_protocol"
                break

            # `entry` is this loop iteration's only raw, unnormalized value; it is discarded
            # (never accumulated, written, printed, or logged) before the next line is read.
            #
            # Every field the rest of the loop body will use (including every field of every
            # entry in `accesses`) must be fully validated -- converted to its final usable
            # type/shape -- inside this one guarded block, never partially validated here and
            # then re-accessed/converted later unguarded. A wrong-shape (but syntactically valid
            # JSON) protocol line from an untrusted adapter must always become
            # AdapterFailure("malformed_protocol"), never an uncaught exception or a
            # CalledProcessError from a downstream probe subprocess call.
            try:
                entry = json.loads(line)
                if not isinstance(entry, dict):
                    raise TypeError("entry must be an object")
                if set(entry) != {"ordinal", "pc", "primary", "extension", "length", "accesses"}:
                    raise TypeError("entry has an invalid field set")
                ordinal = _parse_protocol_uint(entry["ordinal"], MAX_PROTOCOL_ORDINAL)
                primary_hex = _parse_protocol_hex(entry["primary"], 4)
                extension = entry.get("extension")
                extension_hex = "-" if extension is None else _parse_protocol_hex(extension, 8)
                primary_value = int(primary_hex, 16)
                extension_value = int(extension_hex, 16) if extension_hex != "-" else None
                length = _parse_protocol_uint(entry["length"], MAX_PROTOCOL_LENGTH)
                pc_value = int(_parse_protocol_hex(entry["pc"], 8), 16)
                raw_accesses = entry["accesses"]
                if not isinstance(raw_accesses, list):
                    raise TypeError("accesses must be a list")
                if len(raw_accesses) > MAX_ACCESS_RECORDS_PER_INSTRUCTION:
                    raise TypeError("accesses exceeds the per-instruction limit")
                validated_accesses: list = []
                for access in raw_accesses:
                    if not isinstance(access, dict):
                        raise TypeError("access must be an object")
                    if set(access) != {"ordinal", "kind", "address", "byteWidth"}:
                        raise TypeError("access has an invalid field set")
                    access_ordinal = _parse_protocol_uint(
                        access["ordinal"], MAX_PROTOCOL_ORDINAL,
                    )
                    kind = access["kind"]
                    if kind not in VALID_ACCESS_KINDS:
                        raise TypeError("access kind is invalid")
                    width = access["byteWidth"]
                    if (not isinstance(width, int) or isinstance(width, bool)
                            or width not in VALID_ACCESS_BYTE_WIDTHS):
                        raise TypeError("access byteWidth is invalid")
                    address_hex = _parse_protocol_hex(access["address"], 8)
                    validated_accesses.append((access_ordinal, kind, address_hex, width))
            except (json.JSONDecodeError, KeyError, TypeError, ValueError):
                failure_reason = "malformed_protocol"
                break

            classification = stage1_classifier.classify(primary_value, extension_value)
            decoded = _probe_decode(executable, primary_hex, extension_hex)
            support = decoded["support"]

            key = (
                classification["family"], classification["size"], classification["addressingMode"],
                classification["sourceAddressingMode"], classification["destinationAddressingMode"],
                support,
            )
            if key not in instructions_aggregated:
                instructions_aggregated[key] = {
                    "family": classification["family"],
                    "size": classification["size"],
                    "addressingMode": classification["addressingMode"],
                    "sourceAddressingMode": classification["sourceAddressingMode"],
                    "destinationAddressingMode": classification["destinationAddressingMode"],
                    "support": support,
                    "firstObservedOrdinal": ordinal,
                    "observationCount": 0,
                }
                instructions_order.append(key)
            instructions_aggregated[key]["observationCount"] += 1

            hardware_frontier_hit = False
            for access_ordinal, kind, address_hex, width in validated_accesses:
                mapping = _probe_mapping(executable, address_hex, width, length_hex)
                category = mapping["category"]
                if category == "hardware_frontier":
                    hardware_frontier_hit = True
                access_key = (category, kind)
                if access_key not in accesses_aggregated:
                    accesses_aggregated[access_key] = {
                        "category": category,
                        "kind": kind,
                        "firstObservedOrdinal": access_ordinal,
                        "observationCount": 0,
                    }
                    accesses_order.append(access_key)
                accesses_aggregated[access_key]["observationCount"] += 1

            total_instructions += 1
            unique_pcs.add(pc_value)
            if previous_pc is not None and previous_length is not None:
                if pc_value != (previous_pc + previous_length) & 0xFFFFFFFF:
                    control_flow_depth += 1
            previous_pc = pc_value
            previous_length = length

            stop_reason = decide_stop_reason(
                hardware_frontier_hit, total_instructions, len(unique_pcs), control_flow_depth,
            )

            reply = "stop" if stop_reason is not None else "continue"
            try:
                process.stdin.write(reply + "\n")
                process.stdin.flush()
            except (BrokenPipeError, OSError):
                failure_reason = "broken_pipe"
                break

            if reply == "stop":
                stop_sent = True
                break
    finally:
        _bounded_adapter_shutdown(process)
        stderr_text = ""

    if failure_reason is not None:
        raise AdapterFailure(failure_reason, adapter_stderr=stderr_text)
    if not stop_sent:
        # Defensive: unreachable given the loop's own control flow (every other exit path sets
        # failure_reason), but keeps the "raise AdapterFailure or return fully successful" contract
        # airtight against future refactors of the loop above.
        raise AdapterFailure("unexpected_eof_before_stop", adapter_stderr=stderr_text)
    if process.returncode != 0:
        raise AdapterFailure("nonzero_exit", adapter_stderr=stderr_text)

    normalized = {
        "instructions": [instructions_aggregated[key] for key in instructions_order],
        "accesses": [accesses_aggregated[key] for key in accesses_order],
        "stopReason": stop_reason,
        "bounds": {
            "maxExecutedInstructions": MAX_EXECUTED_INSTRUCTIONS,
            "maxUniqueVisitedPcs": MAX_UNIQUE_VISITED_PCS,
            "maxControlFlowDepth": MAX_CONTROL_FLOW_DEPTH,
        },
    }
    return normalized, stderr_text


def write_cache(normalized: dict, cache_path: pathlib.Path) -> None:
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(normalized, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def summarize(normalized: dict) -> str:
    support_counts: "dict[str, int]" = {}
    for instruction in normalized["instructions"]:
        support_counts[instruction["support"]] = (
            support_counts.get(instruction["support"], 0) + instruction["observationCount"]
        )
    category_counts: "dict[str, int]" = {}
    for access in normalized["accesses"]:
        category_counts[access["category"]] = (
            category_counts.get(access["category"], 0) + access["observationCount"]
        )
    gap_observed = any(
        instruction["family"] == "semantic_classifier_gap" for instruction in normalized["instructions"]
    )
    lines = [
        "sonic startup inventory: bounded scan complete",
        f"stop reason: {normalized['stopReason']}",
        f"semantic classifier gap observed: {'yes' if gap_observed else 'no'}",
    ]
    for support in sorted(support_counts):
        lines.append(f"support {support}: {support_counts[support]}")
    for category in sorted(category_counts):
        lines.append(f"category {category}: {category_counts[category]}")
    return "\n".join(lines)


def decide_exit_code(normalized: dict) -> int:
    """Decide the final --scan exit code from a completed normalized inventory.

    Factored out of main() so this decision is directly unit-testable without driving the whole
    --scan CLI path synthetically (which would require a real ROM/adapter). A scan completing
    successfully but observing at least one "semantic_classifier_gap" instruction entry is a
    diagnostic success but a backlog-generation failure: the cache file and summary are still
    written/printed, but this function reports EXIT_SEMANTIC_GAP instead of EXIT_SUCCESS.
    """
    gap_observed = any(
        instruction["family"] == "semantic_classifier_gap" for instruction in normalized["instructions"]
    )
    return EXIT_SEMANTIC_GAP if gap_observed else EXIT_SUCCESS


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sonic_startup_inventory.py",
        description=HELP_TEXT,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--executable", help="path to the built segarecomp CLI executable")
    parser.add_argument("--scan", action="store_true", help="perform one bounded local scan")
    return parser


def main(argv: Optional[list] = None, cache_path: Optional[pathlib.Path] = None) -> int:
    """``cache_path`` defaults to the real gitignored local cache path

    (``PROJECT_ROOT / CACHE_RELATIVE_PATH``); it is overridable purely for test isolation and does
    not change the real default path or behavior for any normal invocation.
    """
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if not args.scan:
        parser.print_help()
        return EXIT_SUCCESS

    if not args.executable:
        print("sonic startup inventory: --scan requires --executable", file=sys.stderr)
        return EXIT_USAGE

    rom_path, rom_error = check_rom_gate()
    if rom_error is not None:
        print(f"sonic startup inventory: {rom_error}", file=sys.stderr)
        return EXIT_ROM_UNAVAILABLE

    adapter_path, musashi_error = check_musashi_gate()
    if musashi_error is not None:
        print(f"sonic startup inventory: {musashi_error}", file=sys.stderr)
        return EXIT_MUSASHI_UNAVAILABLE

    assert rom_path is not None and adapter_path is not None
    image_length = rom_path.stat().st_size
    try:
        normalized, _adapter_stderr = run_scan(adapter_path, rom_path, args.executable, image_length)
    except AdapterFailure as error:
        # Never print error.adapter_stderr: it may contain ROM-derived content from a crashing
        # adapter. No cache file is written -- any previous run's cache is left untouched.
        print(f"sonic startup inventory: adapter failure ({error.reason})", file=sys.stderr)
        return EXIT_ADAPTER_FAILURE

    resolved_cache_path = cache_path if cache_path is not None else PROJECT_ROOT / CACHE_RELATIVE_PATH
    write_cache(normalized, resolved_cache_path)
    print(summarize(normalized))
    return decide_exit_code(normalized)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - never crash with a traceback.
        print(f"sonic startup inventory: unexpected error ({type(error).__name__})", file=sys.stderr)
        sys.exit(1)
