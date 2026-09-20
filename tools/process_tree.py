"""SEG-018-T006: the smallest host seam for child process-tree lifecycle.

POSIX hosts keep using a fresh session plus process-group signalling. Windows has no
process groups or ``select`` on pipes, so it uses a native Job Object (kills every
descendant, including orphans whose parent already exited) and a reader thread for
bounded timed reads. Semantics preserved on both hosts: a bounded read with timeout,
termination of the whole descendant tree, and no pipe leak.
"""

from __future__ import annotations

import os
import queue
import signal
import subprocess
import sys
import threading
from typing import Optional

IS_WINDOWS = os.name == "nt"

if not IS_WINDOWS:
    import select


def popen_kwargs() -> dict:
    """Extra Popen keyword arguments that place the child in its own killable tree."""
    return {} if IS_WINDOWS else {"start_new_session": True}


def script_argv(path: "os.PathLike[str] | str") -> list:
    """argv prefix for a project-authored script/adapter.

    POSIX executes shebang scripts directly. Windows cannot, so a file starting with a
    ``#!`` line naming python is launched through the current interpreter.
    """
    text = os.fspath(path)
    if IS_WINDOWS:
        try:
            with open(text, "rb") as handle:
                first = handle.readline(256)
        except OSError:
            return [text]
        if first.startswith(b"#!") and b"python" in first:
            return [sys.executable, text]
    return [text]


# ---------------------------------------------------------------- Windows job objects
_JOBS: "dict[int, int]" = {}
_JOBS_LOCK = threading.Lock()

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    _JobObjectExtendedLimitInformation = 9
    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000

    class _IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class _BasicLimit(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                    ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class _ExtendedLimit(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", _BasicLimit),
                    ("IoInfo", _IoCounters),
                    ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _k32.CreateJobObjectW.restype = wintypes.HANDLE
    _k32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
    _k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
    _k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _k32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.OpenProcess.restype = wintypes.HANDLE
    _k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]


def attach(process: "subprocess.Popen") -> None:
    """Bind a freshly started child (and all its future descendants) to a kill-on-close job."""
    if not IS_WINDOWS:
        return
    job = _k32.CreateJobObjectW(None, None)
    if not job:
        return
    info = _ExtendedLimit()
    info.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if (not _k32.SetInformationJobObject(job, _JobObjectExtendedLimitInformation,
                                         ctypes.byref(info), ctypes.sizeof(info)) or
            not _k32.AssignProcessToJobObject(job, wintypes.HANDLE(int(process._handle)))):  # type: ignore[attr-defined]
        _k32.CloseHandle(job)
        return
    with _JOBS_LOCK:
        _JOBS[process.pid] = job


def terminate(process: "subprocess.Popen", force: bool) -> None:
    """Terminate the child's whole descendant tree; a vanished tree is not an error."""
    if IS_WINDOWS:
        with _JOBS_LOCK:
            job = _JOBS.get(process.pid)
        if job:
            _k32.TerminateJobObject(job, 1)
        elif process.poll() is None:
            try:
                process.kill()
            except OSError:
                pass
        return
    try:
        os.killpg(process.pid, signal.SIGKILL if force else signal.SIGTERM)
    except ProcessLookupError:
        pass


def release(process: "subprocess.Popen") -> None:
    """Free the per-child job handle (closing it also kills any survivor)."""
    if not IS_WINDOWS:
        return
    with _JOBS_LOCK:
        job = _JOBS.pop(process.pid, None)
    if job:
        _k32.CloseHandle(job)


def pid_alive(pid: int) -> bool:
    """Non-destructive liveness probe (``os.kill(pid, 0)`` would terminate on Windows)."""
    if not IS_WINDOWS:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True
    handle = _k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    try:
        code = wintypes.DWORD()
        if not _k32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return False
        return code.value == 259  # STILL_ACTIVE
    finally:
        _k32.CloseHandle(handle)


def kill_pid(pid: int) -> None:
    """Best-effort forced termination of one pid (test cleanup)."""
    if not IS_WINDOWS:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return
    handle = _k32.OpenProcess(0x0001, False, pid)  # PROCESS_TERMINATE
    if handle:
        _k32.TerminateProcess(handle, 1)
        _k32.CloseHandle(handle)


# ---------------------------------------------------------------- bounded timed reads
class _PumpReader:
    """Windows: a daemon thread drains the pipe so the caller can wait with a timeout."""

    def __init__(self, fd: int) -> None:
        self._fd = fd
        self._queue: "queue.Queue[bytes]" = queue.Queue()
        self._pending = b""
        self._eof = False
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self) -> None:
        while True:
            try:
                chunk = os.read(self._fd, 4096)
            except OSError:
                chunk = b""
            self._queue.put(chunk)
            if not chunk:
                return

    def read(self, limit: int, timeout: float) -> Optional[bytes]:
        if not self._pending and not self._eof:
            try:
                chunk = self._queue.get(timeout=max(0.0, timeout))
            except queue.Empty:
                return None
            if chunk:
                self._pending = chunk
            else:
                self._eof = True
        data, self._pending = self._pending[:limit], self._pending[limit:]
        return data


_READERS: "dict[int, _PumpReader]" = {}


def read_chunk(stream: object, limit: int, timeout: float) -> Optional[bytes]:
    """Read up to ``limit`` bytes; ``None`` on timeout, ``b""`` on EOF."""
    fd = stream.fileno()  # type: ignore[attr-defined]
    if not IS_WINDOWS:
        ready, _, _ = select.select([stream], [], [], timeout)
        if not ready:
            return None
        return os.read(fd, limit)
    with _JOBS_LOCK:
        reader = _READERS.get(fd)
        if reader is None:
            reader = _READERS[fd] = _PumpReader(fd)
    return reader.read(limit, timeout)


def forget(stream: object) -> None:
    """Drop the timed-read state for a stream that is about to be closed."""
    if not IS_WINDOWS:
        return
    try:
        fd = stream.fileno()  # type: ignore[attr-defined]
    except (OSError, ValueError):
        return
    with _JOBS_LOCK:
        _READERS.pop(fd, None)
