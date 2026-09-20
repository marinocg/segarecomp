#!/usr/bin/env python3
"""Minimal newline-delimited JSON-RPC 2.0 client for the Ghidra MCP stdio bridge.

The project-supported agent path is the exact command OpenCode/Claude Code launch:

    python3 tools/ghidra.py stdio

which execs ``bridge-mcp-ghidra --transport stdio --no-lazy`` inside the already
running bridge container. This helper speaks just enough of the MCP wire protocol
(newline-delimited JSON-RPC, which is what the pinned bridge emits) to:

* complete ``initialize`` / ``notifications/initialized``;
* enumerate tools;
* call tools and collect their textual content.

It is intentionally dependency-free (no ``mcp`` SDK on the host) so the tooling
tests can run in a bare checkout. Docker/Compose availability is the caller's
concern; this module only manages the subprocess pipe.
"""

from __future__ import annotations

import json
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]


class McpError(RuntimeError):
    """A JSON-RPC error or transport failure from the bridge."""


class GhidraMcpClient:
    """Drive ``tools/ghidra.py stdio`` as a newline-delimited JSON-RPC peer."""

    def __init__(
        self,
        *,
        runtime_root: Path | None = None,
        timeout: float = 120.0,
        checkout_root: Path | None = None,
    ) -> None:
        self._timeout = timeout
        root = checkout_root or PROJECT_ROOT
        command = [sys.executable, str(root / "tools" / "ghidra.py")]
        if runtime_root is not None:
            command += ["--runtime-root", str(runtime_root)]
        command.append("stdio")
        self._proc = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            cwd=str(root),
        )
        self._next_id = 0
        self._stderr_chunks: list[str] = []
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()

    def _drain_stderr(self) -> None:
        assert self._proc.stderr is not None
        for line in self._proc.stderr:
            self._stderr_chunks.append(line)

    @property
    def stderr_text(self) -> str:
        return "".join(self._stderr_chunks)

    def _send(self, payload: dict[str, Any]) -> None:
        assert self._proc.stdin is not None
        self._proc.stdin.write(json.dumps(payload) + "\n")
        self._proc.stdin.flush()

    def _read_message(self) -> dict[str, Any]:
        assert self._proc.stdout is not None
        result: list[dict[str, Any]] = []

        def _reader() -> None:
            for line in self._proc.stdout:  # type: ignore[union-attr]
                line = line.strip()
                if not line:
                    continue
                try:
                    result.append(json.loads(line))
                except json.JSONDecodeError:
                    continue
                return

        thread = threading.Thread(target=_reader, daemon=True)
        thread.start()
        thread.join(self._timeout)
        if not result:
            raise McpError(
                f"no JSON-RPC response within {self._timeout}s; stderr:\n{self.stderr_text}"
            )
        return result[0]

    def request(self, method: str, params: dict[str, Any] | None = None) -> Any:
        self._next_id += 1
        request_id = self._next_id
        self._send(
            {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}
        )
        while True:
            message = self._read_message()
            if message.get("id") != request_id:
                continue
            if "error" in message:
                raise McpError(f"{method} failed: {message['error']}")
            return message.get("result")

    def notify(self, method: str, params: dict[str, Any] | None = None) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params or {}})

    def initialize(self) -> dict[str, Any]:
        result = self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "segarecomp-smoke", "version": "0"},
            },
        )
        self.notify("notifications/initialized")
        return result

    def list_tools(self) -> list[dict[str, Any]]:
        tools: list[dict[str, Any]] = []
        cursor: str | None = None
        while True:
            params = {"cursor": cursor} if cursor else {}
            result = self.request("tools/list", params)
            tools.extend(result.get("tools", []))
            cursor = result.get("nextCursor")
            if not cursor:
                return tools

    def call_tool(self, name: str, arguments: dict[str, Any] | None = None) -> str:
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        chunks: list[str] = []
        for item in result.get("content", []):
            if item.get("type") == "text":
                chunks.append(item.get("text", ""))
        if result.get("isError"):
            raise McpError(f"{name} returned error: {''.join(chunks)}")
        return "".join(chunks)

    def close(self) -> None:
        try:
            if self._proc.stdin is not None:
                self._proc.stdin.close()
        except OSError:
            pass
        try:
            self._proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait(timeout=5)
        self._stderr_thread.join(timeout=5)
        for stream in (self._proc.stdout, self._proc.stderr):
            try:
                if stream is not None:
                    stream.close()
            except OSError:
                pass

    def __enter__(self) -> "GhidraMcpClient":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


if __name__ == "__main__":
    with GhidraMcpClient() as client:
        info = client.initialize()
        print("server:", info.get("serverInfo"))
        names = [tool["name"] for tool in client.list_tools()]
        print(f"{len(names)} tools")
        for name in names:
            print(" ", name)
