#!/usr/bin/env python3
"""SEG-018-T006: focused proof of the private inherited-writer report transport.

A project-authored C program uses the same opener contract as the generated bridge
(POSIX fd, or on Windows an inherited HANDLE converted to a CRT fd in the child).
The real driver launch helpers pass exactly the requested writers. Proves: the full
channel, the ephemeral channel, both at once (independent), a malformed/non-inherited
token failing closed, and that no report file is created.
"""
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile

C_SOURCE = r'''#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif
static FILE *open_inherited(const char *token) {
  char *end; unsigned long long value;
  if (token == NULL || *token < '0' || *token > '9') return NULL;
  value = strtoull(token, &end, 10);
  if (*end != '\0') return NULL;
#if defined(_WIN32)
  { int fd = _open_osfhandle((intptr_t)(uintptr_t)value, _O_WRONLY | _O_BINARY);
    FILE *stream = fd < 0 ? NULL : _fdopen(fd, "wb");
    if (fd >= 0 && stream == NULL) _close(fd);
    return stream; }
#else
  if (value > 2147483647ULL) return NULL;
  return fdopen((int)value, "w");
#endif
}
int main(int argc, char **argv) {
  int i;
  for (i = 1; i + 1 < argc; i += 2) {
    FILE *out = open_inherited(argv[i + 1]);
    if (out == NULL) return 1;
    fputs(strcmp(argv[i], "--full") == 0 ? "FULL-BYTES\n" : "EPHEMERAL-BYTES\n", out);
    if (fclose(out) != 0) return 1;
  }
  return 0;
}
'''


def load_driver(root):
    spec = importlib.util.spec_from_file_location("bridge_driver", root / "tools" / "genesis_startup_bridge.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_all(fd):
    data = b""
    while True:
        chunk = os.read(fd, 65536)
        if not chunk:
            return data
        data += chunk


def main() -> int:
    compiler, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    driver = load_driver(root)
    with tempfile.TemporaryDirectory() as directory:
        work = pathlib.Path(directory)
        (work / "t.c").write_text(C_SOURCE, encoding="utf-8", newline="\n")
        exe = work / ("t.exe" if os.name == "nt" else "t")
        built = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                "-o", str(exe), str(work / "t.c")], text=True, capture_output=True, cwd=work)
        assert built.returncode == 0, built.stderr

        def run(kinds, bad_token=None):
            pipes = {kind: os.pipe() for kind in kinds}
            try:
                argv = [str(exe)]
                for kind, (_, write_fd) in pipes.items():
                    argv += [f"--{kind}", bad_token or driver._writer_token(write_fd)]
                with driver._inherit_writers(tuple(w for _, w in pipes.values())) as kwargs:
                    child = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=work, **kwargs)
                for _, write_fd in pipes.values():
                    os.close(write_fd)
                got = {kind: read_all(read_fd) for kind, (read_fd, _) in pipes.items()}
                child.communicate(timeout=60)
                return child.returncode, got
            finally:
                for read_fd, _ in pipes.values():
                    os.close(read_fd)

        code, got = run(["full"])
        assert code == 0 and got == {"full": b"FULL-BYTES\n"}, (code, got)
        code, got = run(["ephemeral"])
        assert code == 0 and got == {"ephemeral": b"EPHEMERAL-BYTES\n"}, (code, got)
        code, got = run(["full", "ephemeral"])
        assert code == 0 and got == {"full": b"FULL-BYTES\n", "ephemeral": b"EPHEMERAL-BYTES\n"}, (code, got)
        for bad in ("notanumber", "-1", "7trailing", "999999999999999999999999999999", "4242"):
            code, got = run(["full"], bad_token=bad)
            assert code == 1 and got == {"full": b""}, (bad, code, got)
        assert sorted(p.name for p in work.iterdir()) == sorted(["t.c", exe.name]), "unexpected report file"
    print("genesis_report_transport_test: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
