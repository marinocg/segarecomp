#!/usr/bin/env python3
"""Focused C7 driver test for in-process differing full reports."""
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile


SENTINEL = "028757b2"  # 42424242: deliberately present only in full runtime state.


def fail(kind: str, message: str) -> int:
    sys.stderr.write(f"{kind} failure: {message}\n")
    return 1


def load_driver(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("genesis_startup_bridge", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load driver")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_proxy(path: pathlib.Path) -> None:
    # The only generated text substitution is the already validated digest.
    # Keep this raw template separate from the proxy's argument parser so this
    # test cannot accidentally exercise production emission behavior.
    template = r'''#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <sys/types.h>
#include <unistd.h>
#endif

static FILE *open_inherited(const char *token) {
  unsigned long long value = strtoull(token, NULL, 10);
#if defined(_WIN32)
  int fd = _open_osfhandle((intptr_t)(uintptr_t)value, _O_WRONLY | _O_BINARY);
  FILE *stream = fd < 0 ? NULL : _fdopen(fd, "wb");
  if (fd >= 0 && stream == NULL) _close(fd);
  return stream;
#else
  return fdopen((int)value, "w");
#endif
}

static const char digest[] = "@DIGEST@";

static void write_sanitized(void) {
  (void)printf("{\\\"schema_version\\\":1,\\\"report_kind\\\":\\\"sanitized\\\",\\\"rom_sha256\\\":\\\"%s\\\",\\\"result\\\":\\\"stop\\\",\\\"stop_class\\\":\\\"instruction_budget_exhausted\\\",\\\"diagnostic_category\\\":\\\"instruction_budget_exhausted\\\",\\\"cpu_dimensions\\\":null,\\\"c4_lowering_dimensions\\\":null}\\n", digest);
}

static void write_full(FILE *out) {
  const uint32_t pid_value = (uint32_t)(unsigned long)getpid();
  uint32_t index;
  (void)fprintf(out, "{\\\"schema_version\\\":1,\\\"report_kind\\\":\\\"full\\\",\\\"rom_sha256\\\":\\\"%s\\\",\\\"result\\\":\\\"stop\\\",\\\"runtime\\\":{\\\"d\\\":[\\\"0x%08" PRIx32 "\\\",\\\"0x@SENTINEL@\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\"],\\\"a\\\":[\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\",\\\"0x00000000\\\"],\\\"usp\\\":\\\"0x00000000\\\",\\\"sr\\\":\\\"0x0000\\\",\\\"pc\\\":\\\"0x00000000\\\",\\\"work_ram_base64\\\":\\\"", digest, pid_value);
  for (index = 0; index < 87382U; ++index) {
    (void)fputc('A', out);
  }
  (void)fputs("==\\\"},\\\"stop_class\\\":\\\"instruction_budget_exhausted\\\",\\\"diagnostic_category\\\":\\\"instruction_budget_exhausted\\\",\\\"c4_lowering_dimensions\\\":null,\\\"provenance\\\":{\\\"has_instruction_provenance\\\":false,\\\"instruction\\\":null,\\\"has_access\\\":false,\\\"access_address\\\":\\\"0x00000000\\\",\\\"access_width\\\":null,\\\"access_direction\\\":null,\\\"mapping_claim_count\\\":0,\\\"mapping_claims\\\":[],\\\"bus_access_count\\\":0,\\\"bus_accesses\\\":[]}}\\n", out);
}

int main(int argc, char **argv) {
  FILE *full = NULL;
  int index;
  for (index = 1; index < argc; ++index) {
    if (strcmp(argv[index], "--full-report-fd") == 0 && index + 1 < argc) {
      full = open_inherited(argv[++index]);
    } else if (strcmp(argv[index], "--full-report-path") == 0 && index + 1 < argc) {
      full = fopen(argv[++index], "w");
    }
  }
  write_sanitized();
  if (full != NULL) {
    write_full(full);
    (void)fclose(full);
  }
  return 0;
}
'''
    # The raw Python literal keeps the C template readable; reduce its doubled
    # escaping to C string escapes before substitution.
    template = template.replace(r'\\\"', r'\"')
    template = template.replace(r'\\n', r'\n')
    proxy = r'''#!/usr/bin/env python3
import re
import sys

template = @TEMPLATE@
sentinel = @SENTINEL_LITERAL@
if len(sys.argv) < 2 or sys.argv[1] != "emit-general-startup-bridge-c":
    raise SystemExit(2)
try:
    digest = sys.argv[sys.argv.index("--rom-sha256") + 1]
except (ValueError, IndexError):
    raise SystemExit(2)
if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
    raise SystemExit(2)
sys.stdout.write(template.replace("@DIGEST@", digest).replace("@SENTINEL@", sentinel))
'''.replace("@TEMPLATE@", repr(template)).replace("@SENTINEL_LITERAL@", repr(SENTINEL))
    path.write_text(proxy)
    path.chmod(0o755)


def proxy_smoke(proxy: pathlib.Path, compiler: pathlib.Path, temporary: pathlib.Path,
                bridge_driver) -> tuple[bool, str]:
    digest = hashlib.sha256(b"proxy-smoke").hexdigest()
    generated = subprocess.run([*bridge_driver.process_tree.script_argv(proxy), "emit-general-startup-bridge-c", "--rom", "unused",
                                "--entry", "00000000", "--rom-sha256", digest], text=True,
                               capture_output=True, check=False)
    if generated.returncode != 0 or generated.stderr or digest not in generated.stdout:
        return False, "generation or digest substitution failed"
    source = temporary / "proxy-smoke.c"
    executable = temporary / "proxy-smoke"
    source.write_text(generated.stdout)
    compiled = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                               "-o", str(executable), str(source)], text=True, capture_output=True, check=False)
    if compiled.returncode != 0:
        return False, "strict C11 compilation failed: " + compiled.stderr.strip()
    read_fd, write_fd = os.pipe()
    try:
        with bridge_driver._inherit_writers((write_fd,)) as inherit_kwargs:
            child = subprocess.Popen([str(executable), "--full-report-fd", bridge_driver._writer_token(write_fd)],
                                     text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **inherit_kwargs)
        os.close(write_fd)
        write_fd = -1
        full = b""
        while True:
            chunk = os.read(read_fd, 65536)
            if not chunk:
                break
            full += chunk
        stdout, stderr = child.communicate()
    finally:
        if write_fd >= 0:
            os.close(write_fd)
        os.close(read_fd)
    try:
        sanitized = json.loads(stdout)
        parsed_full = json.loads(full)
    except json.JSONDecodeError:
        return False, "generated reports were not JSON"
    if (child.returncode != 0 or stderr or json.dumps(sanitized, separators=(",", ":")) + "\n" != stdout or
            json.dumps(parsed_full, separators=(",", ":")) + "\n" != full.decode() or
            sanitized.get("rom_sha256") != digest or parsed_full.get("rom_sha256") != digest or
            parsed_full.get("runtime", {}).get("d", [None, None])[1] != "0x" + SENTINEL or
            not bridge_driver.valid_sanitized(sanitized, digest) or
            not bridge_driver.valid_full(parsed_full, sanitized, digest)):
        return False, "canonical full report validation failed"
    return True, ""


def expected_artifacts(directory: pathlib.Path) -> bool:
    return {item.relative_to(directory).as_posix() for item in directory.rglob("*") if item.is_file()} == {
        "bridge.generated.c", "bridge"}


def run_driver_case(driver: pathlib.Path, proxy: pathlib.Path, compiler: pathlib.Path, rom: pathlib.Path,
                    mode: str, output: pathlib.Path, digest: str) -> tuple[bool, str]:
    output.mkdir()
    before = set(output.rglob("*"))
    command = [sys.executable, str(driver), "--segarecomp", str(proxy), "--cc", str(compiler),
               "--rom", str(rom), "--mode", mode, "--out-dir", str(output), "--compare-runs"]
    # Commercial invocation must exercise the canonical reset-entry route;
    # only project-authored synthetic fixtures may select an explicit entry.
    if mode == "synthetic":
        command += ["--entry", "00000000"]
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    after = set(output.rglob("*"))
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        return False, "driver stdout was not JSON"
    expected = {"schema_version": 1, "report_kind": "sanitized", "rom_sha256": digest, "result": "stop",
                 "stop_class": "instruction_budget_exhausted", "diagnostic_category": "instruction_budget_exhausted",
                 "cpu_dimensions": None, "c4_lowering_dimensions": None, "reports_match": False}
    private = ("runtime", "provenance", "work_ram", SENTINEL, str(rom), str(output))
    if (result.returncode != 7 or result.stderr or before or report != expected or
            json.dumps(report, separators=(",", ":")) + "\n" != result.stdout or
            any(value in result.stdout or value in result.stderr for value in private)):
        return False, "unexpected exit, canonical sanitized report, or private output"
    if not expected_artifacts(output) or any("full" in item.name or "compare" in item.name for item in after):
        return False, "full report or comparison artifact persisted"
    return True, ""


def main() -> int:
    if len(sys.argv) != 4:
        return 2
    _, compiler_arg, root_arg = sys.argv[1:]
    compiler, root = pathlib.Path(compiler_arg).resolve(), pathlib.Path(root_arg).resolve()
    driver = root / "tools" / "genesis_startup_bridge.py"
    try:
        bridge_driver = load_driver(driver)
    except (ImportError, OSError, RuntimeError) as error:
        return fail("proxy", str(error))
    with tempfile.TemporaryDirectory(dir=root / "build", prefix="genesis-startup-bridge-c7-") as directory:
        temporary = pathlib.Path(directory)
        proxy = temporary / "test-only-proxy.py"
        write_proxy(proxy)
        passed, message = proxy_smoke(proxy, compiler, temporary, bridge_driver)
        if not passed:
            return fail("proxy", message)
        rom = temporary / "synthetic.bin"
        rom.write_bytes(b"project-authored-c7")
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()
        for mode in ("synthetic", "commercial"):
            output = temporary / f"{mode}-compare"
            passed, message = run_driver_case(driver, proxy, compiler, rom, mode, output, digest)
            if not passed:
                return fail("driver", f"{mode}: {message}")
    print("genesis startup bridge C7 compare-runs privacy: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
