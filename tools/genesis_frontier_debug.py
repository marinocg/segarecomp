#!/usr/bin/env python3
"""Inspect a generated Genesis bridge stop with LLDB/GDB.

Output is private-session diagnostic material. Do not copy it into durable evidence.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys


LLDB_EXPRESSIONS = (
    "address",
    "width",
    "direction",
    "routed_value",
    "runtime->devices.vdp.control_port_awaiting_second_word",
    "runtime->devices.vdp.control_port_first_word",
    "runtime->devices.vdp.addressed_pointer",
    "runtime->devices.vdp.auto_increment_value",
    "runtime->devices.vdp.registers",
    "runtime->devices.vdp.dma",
)

GDB_EXPRESSIONS = LLDB_EXPRESSIONS


def choose_debugger(requested: str) -> str | None:
    if requested != "auto":
        return shutil.which(requested)
    return shutil.which("lldb") or shutil.which("gdb")


def command(debugger: str, binary: pathlib.Path) -> list[str]:
    name = pathlib.Path(debugger).name
    if "lldb" in name:
        result = [debugger, "--batch", "-o", "breakpoint set -n genesis_access_stop", "-o", "run", "-o", "up"]
        for expression in LLDB_EXPRESSIONS:
            result += ["-o", f"frame variable {expression}"]
        return result + [str(binary)]
    result = [debugger, "--batch", "-ex", "break genesis_access_stop", "-ex", "run", "-ex", "up"]
    for expression in GDB_EXPRESSIONS:
        result += ["-ex", f"print {expression}"]
    return result + [str(binary)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument("--debugger", choices=("auto", "lldb", "gdb"), default="auto")
    args = parser.parse_args()
    binary = args.binary.resolve()
    debugger = choose_debugger(args.debugger)
    if debugger is None:
        print("genesis-frontier-debug: LLDB/GDB unavailable", file=sys.stderr)
        return 2
    if not binary.is_file():
        print("genesis-frontier-debug: debug binary not found", file=sys.stderr)
        return 2
    print("EPHEMERAL DEBUG SESSION: do not copy raw output into durable project evidence", file=sys.stderr)
    try:
        return subprocess.run(command(debugger, binary), check=False).returncode
    except OSError as error:
        print(f"genesis-frontier-debug: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
