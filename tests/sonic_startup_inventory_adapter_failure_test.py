#!/usr/bin/env python3
"""SEG-007-T007 (correction pass 3): fail-closed adapter lifecycle/protocol contract tests.

Fully synthetic, fully hermetic: never touches a real ROM or a real Musashi checkout. Drives
``run_scan`` (and, for the full ``main()`` path, ``tool.main`` with the two local gates
monkeypatched to point at synthetic fixtures instead of a real pinned ROM/Musashi checkout, which
this test cannot satisfy without either) against small, project-authored fake "adapter" scripts
that implement just enough of the documented per-instruction streaming wire protocol to exercise
each scenario in ``AdapterFailure``'s contract:

  1. clean success (a documented stop condition is reached, "stop" is sent, the adapter exits 0);
  2. early/unexpected EOF before any documented stop condition is decided;
  3. a nonzero adapter exit code even after a clean "stop" reply;
  4. a malformed/unparseable protocol line;
  5. failure never overwrites a pre-existing cache file.
"""
import contextlib
import io
import json
import os
import pathlib
import signal
import stat
import subprocess
import sys
import tempfile
import time

PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))
from tools import process_tree  # noqa: E402


def _kill0(pid: int) -> None:
    """Portable ``os.kill(pid, 0)``: raise ProcessLookupError when the pid is gone."""
    if not process_tree.pid_alive(pid):
        raise ProcessLookupError(pid)

from tools import sonic_startup_inventory as tool  # noqa: E402

IMAGE_LENGTH = 0x00010000

# A clean-success fake adapter: emits MAX_EXECUTED_INSTRUCTIONS trivial no-access MOVEQ
# instructions (no access ever leaves the driver at hardware_frontier, so the run legitimately
# stops on max_executed_instructions), blocks correctly for each reply, and exits 0 once it
# receives "stop".
_CLEAN_SUCCESS_ADAPTER = """#!/usr/bin/env python3
import json
import sys

ordinal = 0
pc = 0x00000200
while True:
    line = {
        "ordinal": ordinal, "pc": f"0x{pc:08X}", "primary": "0x7000", "extension": None,
        "length": 2, "accesses": [],
    }
    ordinal += 1
    pc += 2
    sys.stdout.write(json.dumps(line) + "\\n")
    sys.stdout.flush()
    reply = sys.stdin.readline().strip()
    if reply != "continue":
        break
sys.exit(0)
"""

# A control-flow-depth integration fixture: its 33 valid, no-access instructions use PCs four
# bytes apart despite reporting a two-byte length, so every PC after the first is a non-fallthrough
# transition. The scanner must stop exactly on its 32nd transition, well before either 500 limit.
_CONTROL_FLOW_DEPTH_ADAPTER = f"""#!/usr/bin/env python3
import json
import sys

for ordinal in range({tool.MAX_CONTROL_FLOW_DEPTH + 1}):
    line = {{
        "ordinal": ordinal, "pc": f"0x{{0x00000200 + ordinal * 4:08X}}", "primary": "0x7000",
        "extension": None, "length": 2, "accesses": [],
    }}
    sys.stdout.write(json.dumps(line) + "\\n")
    sys.stdout.flush()
    reply = sys.stdin.readline().strip()
    if reply != "continue":
        break
sys.exit(0)
"""

# Emits one or two instructions (never reaching any documented stop condition -- far below
# MAX_EXECUTED_INSTRUCTIONS/MAX_UNIQUE_VISITED_PCS/MAX_CONTROL_FLOW_DEPTH, and no access is
# hardware_frontier) and then exits 0 on its own, without ever receiving/needing a "stop" reply.
_EARLY_EOF_ADAPTER = """#!/usr/bin/env python3
import json
import sys

for ordinal, pc in enumerate((0x00000200, 0x00000202)):
    line = {
        "ordinal": ordinal, "pc": f"0x{pc:08X}", "primary": "0x7000", "extension": None,
        "length": 2, "accesses": [],
    }
    sys.stdout.write(json.dumps(line) + "\\n")
    sys.stdout.flush()
    sys.stdin.readline()
sys.exit(0)
"""

# Behaves correctly through the protocol (one access classified hardware_frontier, so the driver
# legitimately sends "stop"), but then exits nonzero instead of 0.
_NONZERO_EXIT_AFTER_STOP_ADAPTER = """#!/usr/bin/env python3
import json
import sys

line = {
    "ordinal": 0, "pc": "0x00000200", "primary": "0x4E71", "extension": None, "length": 2,
    "accesses": [{"ordinal": 0, "kind": "write", "address": "0x00C00004", "byteWidth": 2}],
}
sys.stdout.write(json.dumps(line) + "\\n")
sys.stdout.flush()
sys.stdin.readline()
sys.exit(1)
"""

# Emits one line of garbage that is not valid JSON at all.
_MALFORMED_NOT_JSON_ADAPTER = """#!/usr/bin/env python3
import sys

sys.stdout.write("this is not json\\n")
sys.stdout.flush()
sys.stdin.readline()
sys.exit(0)
"""

# Emits valid JSON that is missing a required key ("accesses").
_MALFORMED_MISSING_KEY_ADAPTER = """#!/usr/bin/env python3
import json
import sys

line = {"ordinal": 0, "pc": "0x00000200", "primary": "0x7000", "length": 2}
sys.stdout.write(json.dumps(line) + "\\n")
sys.stdout.flush()
sys.stdin.readline()
sys.exit(0)
"""


def _malformed_access_adapter(accesses: object) -> str:
    """Return an adapter emitting one valid-JSON line with a malformed access payload."""
    line = {
        "ordinal": 0, "pc": "0x00000200", "primary": "0x7000", "extension": None,
        "length": 2, "accesses": accesses,
    }
    return f"""#!/usr/bin/env python3
import json
import sys

line = {line!r}
sys.stdout.write(json.dumps(line) + "\\n")
sys.stdout.flush()
sys.stdin.readline()
sys.exit(0)
"""


def _malformed_entry_adapter(entry: object) -> str:
    """Return an adapter emitting one valid-JSON line with a malformed entry payload."""
    return f"""#!/usr/bin/env python3
import json
import sys

line = {entry!r}
sys.stdout.write(json.dumps(line) + "\\n")
sys.stdout.flush()
sys.stdin.readline()
sys.exit(0)
"""


def _hanging_malformed_adapter(pid_path: pathlib.Path) -> str:
    """Emit malformed protocol/stderr, then retain every pipe until terminated."""
    return f"""#!/usr/bin/env python3
import os
import sys
import time

open({str(pid_path)!r}, "w", encoding="utf-8").write(str(os.getpid()))
sys.stdout.write("not-json\\n")
sys.stdout.flush()
sys.stderr.write("ROM-DERIVED-STDERR-MUST-NOT-ESCAPE\\n")
sys.stderr.flush()
while True:
    time.sleep(1)
"""


def _forking_sigterm_ignoring_malformed_adapter(pid_path: pathlib.Path) -> str:
    """Spawn a pipe-holding SIGTERM-ignoring descendant before malformed output.

    A portable ``subprocess`` spawn (not ``os.fork``) so the real descendant-cleanup
    regression runs on every supported host; the descendant explicitly inherits the
    adapter's stdout/stderr pipes.
    """
    descendant = (
        "import os, signal, time\n"
        "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        f"open({str(pid_path)!r}, 'w', encoding='utf-8').write(str(os.getpid()))\n"
        "while True:\n"
        "    time.sleep(1)\n"
    )
    return f"""#!/usr/bin/env python3
import os
import subprocess
import sys
import time

subprocess.Popen([sys.executable, "-c", {descendant!r}], stdout=sys.stdout, stderr=sys.stderr)

while not os.path.exists({str(pid_path)!r}):
    time.sleep(0.001)
sys.stdout.write("not-json\\n")
sys.stdout.flush()
while True:
    time.sleep(1)
"""


def _silent_adapter(pid_path: pathlib.Path) -> str:
    """Start successfully but never send a protocol line or close its pipes."""
    return f"""#!/usr/bin/env python3
import os
import time

open({str(pid_path)!r}, "w", encoding="utf-8").write(str(os.getpid()))
while True:
    time.sleep(1)
"""


def _oversize_partial_line_adapter(pid_path: pathlib.Path) -> str:
    """Write a newline-free protocol record beyond the scanner's literal buffer limit."""
    return f"""#!/usr/bin/env python3
import os
import sys
import time

open({str(pid_path)!r}, "w", encoding="utf-8").write(str(os.getpid()))
sys.stdout.write("x" * {tool.MAX_PROTOCOL_LINE_BYTES + 1})
sys.stdout.flush()
while True:
    time.sleep(1)
"""


def _blank_flood_adapter(pid_path: pathlib.Path) -> str:
    """Flood blank records and retain pipes unless the driver reaps us."""
    return f"""#!/usr/bin/env python3
import os
import sys
import time

open({str(pid_path)!r}, "w", encoding="utf-8").write(str(os.getpid()))
while True:
    sys.stdout.write("   \\n")
    sys.stdout.flush()
    time.sleep(0.001)
"""


def _waiting_valid_adapter(pid_path: pathlib.Path) -> str:
    """Emit one valid record, then wait for the reply a failed probe never sends."""
    return f"""#!/usr/bin/env python3
import json
import os
import sys

open({str(pid_path)!r}, "w", encoding="utf-8").write(str(os.getpid()))
print(json.dumps({{"ordinal": 0, "pc": "0x00000200", "primary": "0x7000", "extension": None,
                  "length": 2, "accesses": []}}), flush=True)
sys.stdin.readline()
"""


def _bad_probe_executable(pid_path: pathlib.Path, mode: str) -> str:
    """A provided production-probe executable which hangs or overproduces stdout."""
    if mode == "hang":
        body = "while True: time.sleep(1)"
    else:
        body = f"sys.stdout.write('x' * {tool.MAX_PROBE_STDOUT_BYTES + 1}); sys.stdout.flush(); time.sleep(10)"
    return f"""#!/usr/bin/env python3
import os
import sys
import time

open({str(pid_path)!r}, "w", encoding="utf-8").write(str(os.getpid()))
{body}
"""


def _write_adapter(tmp_path: pathlib.Path, name: str, source: str) -> pathlib.Path:
    adapter_path = tmp_path / name
    adapter_path.write_text(source, encoding="utf-8")
    adapter_path.chmod(adapter_path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return adapter_path


def _documented_adapter_source() -> str:
    """Extract the local-only adapter exactly as the bootstrap instructions require."""
    document = (PROJECT_ROOT / "docs" / "testing" / "sonic-startup-inventory.md").read_text(
        encoding="utf-8",
    )
    adapter_section = document.split("### Adapter Source\n", 1)[1]
    return adapter_section.split("```c\n", 1)[1].split("\n```", 1)[0]


def test_documented_adapter_ram_boundaries(tmp_path: pathlib.Path) -> None:
    """Compile and exercise the embedded adapter's whole-width RAM checks without Musashi."""
    (tmp_path / "adapter.c").write_text(_documented_adapter_source(), encoding="utf-8")
    (tmp_path / "m68k.h").write_text(
        """#define M68K_CPU_TYPE_68000 1
void m68k_end_timeslice(void);
void m68k_set_cpu_type(unsigned int);
void m68k_init(void);
void m68k_set_instr_hook_callback(void (*)(unsigned int));
void m68k_pulse_reset(void);
int m68k_execute(int);
unsigned int m68k_disassemble(char *, unsigned int, unsigned int);
""",
        encoding="utf-8",
    )
    (tmp_path / "boundary.c").write_text(
        """#include <assert.h>
#include <string.h>
#define main documented_adapter_main
#include "adapter.c"
#undef main
void m68k_end_timeslice(void) {}
void m68k_set_cpu_type(unsigned int value) { (void)value; }
void m68k_init(void) {}
void m68k_set_instr_hook_callback(void (*hook)(unsigned int)) { (void)hook; }
void m68k_pulse_reset(void) {}
int m68k_execute(int cycles) { return cycles; }
unsigned int m68k_disassemble(char *text, unsigned int pc, unsigned int type) {
  (void)text; (void)pc; (void)type; return 2U;
}
int main(void) {
  const uint32_t last = RAM_END - 1U;
  memset(g_ram, 0, sizeof(g_ram));
  g_pending_access_count = 0U;
  m68k_write_memory_8(last, 0x5AU);
  assert(g_ram[sizeof(g_ram) - 1U] == 0x5AU);
  assert(m68k_read_memory_8(last) == 0x5AU);
  m68k_write_memory_16(last, 0x1234U);
  m68k_write_memory_32(RAM_END - 2U, 0x12345678U);
  assert(g_ram[sizeof(g_ram) - 1U] == 0x5AU);
  assert(m68k_read_memory_16(last) == 0xFFFFU);
  assert(m68k_read_memory_32(RAM_END - 2U) == 0xFFFFFFFFU);
  assert(g_pending_access_count == 6U);
  assert(g_pending_accesses[2].address == last && g_pending_accesses[2].byte_width == 2U);
  assert(g_pending_accesses[3].address == RAM_END - 2U &&
         g_pending_accesses[3].byte_width == 4U);
  return 0;
}
""",
        encoding="utf-8",
    )
    executable = tmp_path / "adapter-boundary"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(tmp_path),
         str(tmp_path / "boundary.c"), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)


def _assert_process_absent_with_cleanup(pid_path: pathlib.Path) -> None:
    """Assert eventual absence while ensuring a failing regression never leaks a child."""
    pid = int(pid_path.read_text(encoding="utf-8"))
    deadline = time.monotonic() + tool.ADAPTER_SHUTDOWN_TIMEOUT_SECONDS + 1
    descendant_absent = False
    try:
        while True:
            try:
                _kill0(pid)
            except ProcessLookupError:
                descendant_absent = True
                return
            if time.monotonic() >= deadline:
                raise AssertionError("adapter descendant must be absent after group cleanup")
            time.sleep(0.01)
    finally:
        # If the assertion exposed a regression, avoid leaving the hermetic test's descendant
        # alive in the test runner. A successfully cleaned process simply makes this a no-op.
        if not descendant_absent:
            try:
                process_tree.kill_pid(pid)
            except ProcessLookupError:
                pass


def test_clean_success_run_scan(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(tmp_path, "clean-success-adapter.py", _CLEAN_SUCCESS_ADAPTER)
    rom_path = tmp_path / "unused-rom.bin"
    rom_path.write_bytes(b"\x00")

    normalized, adapter_stderr = tool.run_scan(adapter_path, rom_path, executable, IMAGE_LENGTH)
    assert adapter_stderr == "", adapter_stderr
    assert normalized["stopReason"] == "max_executed_instructions", normalized["stopReason"]
    total_observations = sum(entry["observationCount"] for entry in normalized["instructions"])
    assert total_observations == tool.MAX_EXECUTED_INSTRUCTIONS, total_observations


import contextlib


@contextlib.contextmanager
def generous_protocol_timeout():
    """Success-path cases must not race the 1 s production line timeout under parallel CTest load.

    Timeout-failure cases keep the production value so they still exercise genuine timeouts.
    """
    old = tool.ADAPTER_PROTOCOL_LINE_TIMEOUT_SECONDS
    tool.ADAPTER_PROTOCOL_LINE_TIMEOUT_SECONDS = 30
    try:
        yield
    finally:
        tool.ADAPTER_PROTOCOL_LINE_TIMEOUT_SECONDS = old


def test_control_flow_depth_streaming_stop(executable: str, tmp_path: pathlib.Path) -> None:
    """The streaming path stops on exactly MAX_CONTROL_FLOW_DEPTH non-fallthrough transitions."""
    with generous_protocol_timeout():
        _control_flow_depth_streaming_stop(executable, tmp_path)


def _control_flow_depth_streaming_stop(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(
        tmp_path, "control-flow-depth-adapter.py", _CONTROL_FLOW_DEPTH_ADAPTER,
    )
    rom_path = tmp_path / "unused-rom-control-flow-depth.bin"
    rom_path.write_bytes(b"\x00")

    normalized, adapter_stderr = tool.run_scan(adapter_path, rom_path, executable, IMAGE_LENGTH)
    assert adapter_stderr == "", adapter_stderr
    assert normalized["stopReason"] == "max_control_flow_depth", normalized["stopReason"]
    observations = sum(entry["observationCount"] for entry in normalized["instructions"])
    assert observations == tool.MAX_CONTROL_FLOW_DEPTH + 1, observations
    assert observations - 1 == tool.MAX_CONTROL_FLOW_DEPTH
    assert observations < tool.MAX_EXECUTED_INSTRUCTIONS
    assert observations < tool.MAX_UNIQUE_VISITED_PCS


def test_stop_decision_exact_thresholds_and_precedence() -> None:
    """Cover each production stop independently despite the two equal 500-value limits."""
    decide = tool.decide_stop_reason
    assert decide(False, tool.MAX_EXECUTED_INSTRUCTIONS - 1,
                  tool.MAX_UNIQUE_VISITED_PCS - 1, tool.MAX_CONTROL_FLOW_DEPTH - 1) is None
    assert decide(False, tool.MAX_EXECUTED_INSTRUCTIONS,
                  tool.MAX_UNIQUE_VISITED_PCS - 1, 0) == "max_executed_instructions"
    assert decide(False, tool.MAX_EXECUTED_INSTRUCTIONS - 1,
                  tool.MAX_UNIQUE_VISITED_PCS, 0) == "max_unique_visited_pcs"
    assert decide(False, 0, 0, tool.MAX_CONTROL_FLOW_DEPTH) == "max_control_flow_depth"
    assert decide(False, tool.MAX_EXECUTED_INSTRUCTIONS - 1,
                  tool.MAX_UNIQUE_VISITED_PCS - 1, tool.MAX_CONTROL_FLOW_DEPTH - 1) is None
    assert decide(True, tool.MAX_EXECUTED_INSTRUCTIONS,
                  tool.MAX_UNIQUE_VISITED_PCS, tool.MAX_CONTROL_FLOW_DEPTH) == "hardware_frontier"
    assert decide(False, tool.MAX_EXECUTED_INSTRUCTIONS,
                  tool.MAX_UNIQUE_VISITED_PCS, tool.MAX_CONTROL_FLOW_DEPTH) == (
        "max_executed_instructions"
    )
    assert decide(False, tool.MAX_EXECUTED_INSTRUCTIONS - 1,
                  tool.MAX_UNIQUE_VISITED_PCS, tool.MAX_CONTROL_FLOW_DEPTH) == (
        "max_unique_visited_pcs"
    )
    assert decide(True, 0, 0, 0) == "hardware_frontier"


def test_help_lists_actual_stop_precedence() -> None:
    """Keep user-facing help synchronized with decide_stop_reason's precedence."""
    positions = [tool.HELP_TEXT.index(text) for text in (
        "1. first observed memory/device access classified \"hardware_frontier\"",
        "2. MAX_EXECUTED_INSTRUCTIONS",
        "3. MAX_UNIQUE_VISITED_PCS",
        "4. MAX_CONTROL_FLOW_DEPTH",
    )]
    assert positions == sorted(positions), positions


def test_clean_success_main_full_path(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(tmp_path, "clean-success-main-adapter.py", _CLEAN_SUCCESS_ADAPTER)
    rom_path = tmp_path / "unused-rom-main.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "clean-success-cache.json"
    assert not cache_path.exists()

    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        exit_code = tool.main(["--executable", executable, "--scan"], cache_path=cache_path)
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert exit_code == tool.EXIT_SUCCESS, exit_code
    assert cache_path.exists(), "successful scan must write the cache file"
    written = json.loads(cache_path.read_text(encoding="utf-8"))
    assert written["stopReason"] == "max_executed_instructions", written["stopReason"]


def test_early_unexpected_eof(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(tmp_path, "early-eof-adapter.py", _EARLY_EOF_ADAPTER)
    rom_path = tmp_path / "unused-rom-eof.bin"
    rom_path.write_bytes(b"\x00")

    try:
        tool.run_scan(adapter_path, rom_path, executable, IMAGE_LENGTH)
        assert False, "expected AdapterFailure"
    except tool.AdapterFailure as error:
        assert error.reason == "unexpected_eof_before_stop", error.reason


def test_early_unexpected_eof_main_no_cache_write(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(tmp_path, "early-eof-main-adapter.py", _EARLY_EOF_ADAPTER)
    rom_path = tmp_path / "unused-rom-eof-main.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "early-eof-cache.json"
    sentinel = '{"sentinel": true}\n'
    cache_path.write_text(sentinel, encoding="utf-8")

    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        exit_code = tool.main(["--executable", executable, "--scan"], cache_path=cache_path)
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert exit_code == tool.EXIT_ADAPTER_FAILURE, exit_code
    assert cache_path.read_text(encoding="utf-8") == sentinel, "must never overwrite an existing cache"


def test_nonzero_exit_after_clean_stop(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(
        tmp_path, "nonzero-exit-adapter.py", _NONZERO_EXIT_AFTER_STOP_ADAPTER,
    )
    rom_path = tmp_path / "unused-rom-nonzero.bin"
    rom_path.write_bytes(b"\x00")

    try:
        tool.run_scan(adapter_path, rom_path, executable, IMAGE_LENGTH)
        assert False, "expected AdapterFailure"
    except tool.AdapterFailure as error:
        assert error.reason == "nonzero_exit", error.reason


def test_malformed_protocol_not_json(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(
        tmp_path, "malformed-not-json-adapter.py", _MALFORMED_NOT_JSON_ADAPTER,
    )
    rom_path = tmp_path / "unused-rom-malformed1.bin"
    rom_path.write_bytes(b"\x00")

    try:
        tool.run_scan(adapter_path, rom_path, executable, IMAGE_LENGTH)
        assert False, "expected AdapterFailure"
    except tool.AdapterFailure as error:
        assert error.reason == "malformed_protocol", error.reason


def test_malformed_protocol_missing_key(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(
        tmp_path, "malformed-missing-key-adapter.py", _MALFORMED_MISSING_KEY_ADAPTER,
    )
    rom_path = tmp_path / "unused-rom-malformed2.bin"
    rom_path.write_bytes(b"\x00")

    try:
        tool.run_scan(adapter_path, rom_path, executable, IMAGE_LENGTH)
        assert False, "expected AdapterFailure"
    except tool.AdapterFailure as error:
        assert error.reason == "malformed_protocol", error.reason


def test_malformed_access_shapes_fail_before_probes(executable: str, tmp_path: pathlib.Path) -> None:
    """All access fields must be validated before either production probe can run."""
    malformed_accesses = (
        {},
        [[]],
        [{"ordinal": [], "kind": "write", "address": "0x00C00004", "byteWidth": 2}],
        [{"ordinal": 0, "kind": [], "address": "0x00C00004", "byteWidth": 2}],
        [{"ordinal": 0, "kind": "write", "address": [], "byteWidth": 2}],
        [{"ordinal": 0, "kind": "write", "address": "0x00C00004", "byteWidth": []}],
        [{"ordinal": 0, "kind": "write", "address": "0x00C00004", "byteWidth": -1}],
        [{"ordinal": 0, "kind": "private:rom-derived", "address": "0x00C00004", "byteWidth": 2}],
    )

    def unexpected_probe(*_args: object) -> dict:
        raise AssertionError("malformed access payload must not invoke a production probe")

    old_decode, old_mapping = tool._probe_decode, tool._probe_mapping
    try:
        tool._probe_decode = unexpected_probe
        tool._probe_mapping = unexpected_probe
        for index, accesses in enumerate(malformed_accesses):
            adapter_path = _write_adapter(
                tmp_path, f"malformed-access-{index}-adapter.py",
                _malformed_access_adapter(accesses),
            )
            rom_path = tmp_path / f"unused-rom-malformed-access-{index}.bin"
            rom_path.write_bytes(b"\x00")
            try:
                tool.run_scan(adapter_path, rom_path, executable, IMAGE_LENGTH)
                assert False, "expected AdapterFailure"
            except tool.AdapterFailure as error:
                assert error.reason == "malformed_protocol", error.reason
    finally:
        tool._probe_decode, tool._probe_mapping = old_decode, old_mapping


def test_malformed_entry_values_fail_before_probes_and_preserve_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    """Valid JSON protocol violations must not probe or permit a cache write."""
    valid_entry = {
        "ordinal": 0, "pc": "0x00000200", "primary": "0x7000", "extension": None,
        "length": 2, "accesses": [],
    }
    malformed_entries = (
        {**valid_entry, "ordinal": -1},
        {**valid_entry, "length": -1},
        {**valid_entry, "pc": "0x200"},
        {**valid_entry, "primary": "0x07000"},
        {**valid_entry, "extension": "0x1234"},
        {**valid_entry, "unexpected": "value"},
    )
    calls: list[str] = []

    def unexpected_decode(*_args: object) -> dict:
        calls.append("decode")
        raise AssertionError("malformed entry payload must not invoke a production probe")

    def unexpected_mapping(*_args: object) -> dict:
        calls.append("mapping")
        raise AssertionError("malformed entry payload must not invoke a production probe")

    old_decode, old_mapping = tool._probe_decode, tool._probe_mapping
    try:
        tool._probe_decode = unexpected_decode
        tool._probe_mapping = unexpected_mapping
        for index, entry in enumerate(malformed_entries):
            adapter_path = _write_adapter(
                tmp_path, f"malformed-entry-{index}-adapter.py", _malformed_entry_adapter(entry),
            )
            rom_path = tmp_path / f"unused-rom-malformed-entry-{index}.bin"
            rom_path.write_bytes(b"\x00")
            cache_path = tmp_path / f"malformed-entry-{index}-cache.json"
            sentinel = '{"sentinel": "entry-case"}\n'
            cache_path.write_text(sentinel, encoding="utf-8")
            old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
            try:
                tool.check_rom_gate = lambda: (rom_path, None)
                tool.check_musashi_gate = lambda: (adapter_path, None)
                assert tool.main(["--executable", executable, "--scan"], cache_path=cache_path) == (
                    tool.EXIT_ADAPTER_FAILURE
                )
            finally:
                tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate
            assert cache_path.read_text(encoding="utf-8") == sentinel
            assert calls == [], calls
    finally:
        tool._probe_decode, tool._probe_mapping = old_decode, old_mapping


def test_malformed_protocol_main_no_cache_write(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(
        tmp_path, "malformed-main-adapter.py", _MALFORMED_NOT_JSON_ADAPTER,
    )
    rom_path = tmp_path / "unused-rom-malformed-main.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "malformed-cache.json"
    sentinel = '{"sentinel": "malformed-case"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")

    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        exit_code = tool.main(["--executable", executable, "--scan"], cache_path=cache_path)
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert exit_code == tool.EXIT_ADAPTER_FAILURE, exit_code
    assert cache_path.read_text(encoding="utf-8") == sentinel, "must never overwrite an existing cache"


def test_hanging_malformed_adapter_is_bounded_and_preserves_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    pid_path = tmp_path / "hanging-adapter.pid"
    adapter_path = _write_adapter(
        tmp_path, "hanging-malformed-adapter.py", _hanging_malformed_adapter(pid_path),
    )
    rom_path = tmp_path / "unused-rom-hanging-malformed.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "hanging-malformed-cache.json"
    sentinel = '{"sentinel": "hanging-malformed"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")
    captured_stderr = io.StringIO()

    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        started = time.monotonic()
        with contextlib.redirect_stderr(captured_stderr):
            exit_code = tool.main(["--executable", executable, "--scan"], cache_path=cache_path)
        elapsed = time.monotonic() - started
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert exit_code == tool.EXIT_ADAPTER_FAILURE, exit_code
    assert elapsed < tool.ADAPTER_SHUTDOWN_TIMEOUT_SECONDS * 2 + 1, elapsed
    assert "ROM-DERIVED-STDERR-MUST-NOT-ESCAPE" not in captured_stderr.getvalue()
    assert cache_path.read_text(encoding="utf-8") == sentinel
    pid = int(pid_path.read_text(encoding="utf-8"))
    try:
        _kill0(pid)
        assert False, "adapter must be reaped rather than left running/zombie"
    except ProcessLookupError:
        pass


def test_forking_sigterm_ignoring_malformed_adapter_is_group_reaped_and_preserves_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    """Malformed parents cannot strand SIGTERM-ignoring descendants outside cleanup."""
    pid_path = tmp_path / "forking-descendant.pid"
    adapter_path = _write_adapter(
        tmp_path, "forking-malformed-adapter.py",
        _forking_sigterm_ignoring_malformed_adapter(pid_path),
    )
    rom_path = tmp_path / "unused-rom-forking-malformed.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "forking-malformed-cache.json"
    sentinel = '{"sentinel": "forking-malformed"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")

    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        started = time.monotonic()
        with contextlib.redirect_stderr(io.StringIO()):
            exit_code = tool.main(["--executable", executable, "--scan"], cache_path=cache_path)
        elapsed = time.monotonic() - started
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert exit_code == tool.EXIT_ADAPTER_FAILURE, exit_code
    assert elapsed < tool.ADAPTER_SHUTDOWN_TIMEOUT_SECONDS * 2 + 1, elapsed
    assert cache_path.read_text(encoding="utf-8") == sentinel
    _assert_process_absent_with_cleanup(pid_path)


def test_access_list_overflow_fails_before_probes_and_preserves_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    access = {"ordinal": 0, "kind": "write", "address": "0x00C00004", "byteWidth": 2}
    adapter_path = _write_adapter(
        tmp_path, "access-overflow-adapter.py",
        _malformed_access_adapter([access] * (tool.MAX_ACCESS_RECORDS_PER_INSTRUCTION + 1)),
    )
    rom_path = tmp_path / "unused-rom-access-overflow.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "access-overflow-cache.json"
    sentinel = '{"sentinel": "access-overflow"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")
    probe_calls: list[str] = []

    def unexpected_probe(*_args: object) -> dict:
        probe_calls.append("probe")
        raise AssertionError("oversize access list must not invoke a production probe")

    old_decode, old_mapping = tool._probe_decode, tool._probe_mapping
    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool._probe_decode = unexpected_probe
        tool._probe_mapping = unexpected_probe
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        assert tool.main(["--executable", executable, "--scan"], cache_path=cache_path) == (
            tool.EXIT_ADAPTER_FAILURE
        )
    finally:
        tool._probe_decode, tool._probe_mapping = old_decode, old_mapping
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert probe_calls == [], probe_calls
    assert cache_path.read_text(encoding="utf-8") == sentinel


def test_silent_adapter_protocol_wait_is_bounded_and_preserves_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    pid_path = tmp_path / "silent-adapter.pid"
    adapter_path = _write_adapter(tmp_path, "silent-adapter.py", _silent_adapter(pid_path))
    rom_path = tmp_path / "unused-rom-silent.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "silent-cache.json"
    sentinel = '{"sentinel": "silent-adapter"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")

    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        started = time.monotonic()
        exit_code = tool.main(["--executable", executable, "--scan"], cache_path=cache_path)
        elapsed = time.monotonic() - started
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert exit_code == tool.EXIT_ADAPTER_FAILURE, exit_code
    assert elapsed < tool.ADAPTER_PROTOCOL_LINE_TIMEOUT_SECONDS + 2, elapsed
    assert cache_path.read_text(encoding="utf-8") == sentinel
    pid = int(pid_path.read_text(encoding="utf-8"))
    try:
        _kill0(pid)
        assert False, "adapter must be reaped rather than left running/zombie"
    except ProcessLookupError:
        pass


def test_oversize_partial_protocol_line_fails_before_probes_and_preserves_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    pid_path = tmp_path / "oversize-partial-adapter.pid"
    adapter_path = _write_adapter(
        tmp_path, "oversize-partial-adapter.py", _oversize_partial_line_adapter(pid_path),
    )
    rom_path = tmp_path / "unused-rom-oversize-partial.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "oversize-partial-cache.json"
    sentinel = '{"sentinel": "oversize-partial"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")
    probe_calls: list[str] = []
    captured_stderr = io.StringIO()

    def unexpected_probe(*_args: object) -> dict:
        probe_calls.append("probe")
        raise AssertionError("oversize protocol line must not invoke a production probe")

    old_decode, old_mapping = tool._probe_decode, tool._probe_mapping
    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool._probe_decode = unexpected_probe
        tool._probe_mapping = unexpected_probe
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        with contextlib.redirect_stderr(captured_stderr):
            assert tool.main(["--executable", executable, "--scan"], cache_path=cache_path) == (
                tool.EXIT_ADAPTER_FAILURE
            )
    finally:
        tool._probe_decode, tool._probe_mapping = old_decode, old_mapping
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert "adapter failure (malformed_protocol)" in captured_stderr.getvalue()
    assert probe_calls == [], probe_calls
    assert cache_path.read_text(encoding="utf-8") == sentinel
    pid = int(pid_path.read_text(encoding="utf-8"))
    try:
        _kill0(pid)
        assert False, "adapter must be reaped rather than left running/zombie"
    except ProcessLookupError:
        pass


def test_blank_flood_is_malformed_bounded_reaped_and_preserves_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    pid_path = tmp_path / "blank-flood.pid"
    adapter_path = _write_adapter(tmp_path, "blank-flood-adapter.py", _blank_flood_adapter(pid_path))
    rom_path = tmp_path / "unused-rom-blank-flood.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "blank-flood-cache.json"
    sentinel = '{"sentinel": "blank-flood"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")
    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        started = time.monotonic()
        assert tool.main(["--executable", executable, "--scan"], cache_path=cache_path) == (
            tool.EXIT_ADAPTER_FAILURE
        )
        elapsed = time.monotonic() - started
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate
    assert elapsed < tool.ADAPTER_SHUTDOWN_TIMEOUT_SECONDS + 2, elapsed
    assert cache_path.read_text(encoding="utf-8") == sentinel
    try:
        _kill0(int(pid_path.read_text(encoding="utf-8")))
        assert False, "blank-flood adapter must be reaped"
    except ProcessLookupError:
        pass


def test_probe_failures_are_bounded_reaped_and_preserve_cache(
        executable: str, tmp_path: pathlib.Path) -> None:
    for mode, expected_reason in (("hang", "probe_timeout"), ("flood", "probe_output_limit")):
        adapter_pid = tmp_path / f"probe-{mode}-adapter.pid"
        probe_pid = tmp_path / f"probe-{mode}.pid"
        adapter_path = _write_adapter(
            tmp_path, f"probe-{mode}-adapter.py", _waiting_valid_adapter(adapter_pid),
        )
        probe_path = _write_adapter(
            tmp_path, f"probe-{mode}.py", _bad_probe_executable(probe_pid, mode),
        )
        rom_path = tmp_path / f"unused-rom-probe-{mode}.bin"
        rom_path.write_bytes(b"\x00")
        cache_path = tmp_path / f"probe-{mode}-cache.json"
        sentinel = '{"sentinel": "probe-failure"}\n'
        cache_path.write_text(sentinel, encoding="utf-8")
        old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
        try:
            tool.check_rom_gate = lambda: (rom_path, None)
            tool.check_musashi_gate = lambda: (adapter_path, None)
            started = time.monotonic()
            with contextlib.redirect_stderr(io.StringIO()) as captured_stderr:
                assert tool.main(["--executable", str(probe_path), "--scan"], cache_path=cache_path) == (
                    tool.EXIT_ADAPTER_FAILURE
                )
            elapsed = time.monotonic() - started
        finally:
            tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate
        assert expected_reason in captured_stderr.getvalue(), captured_stderr.getvalue()
        assert elapsed < tool.PROBE_TIMEOUT_SECONDS + tool.ADAPTER_SHUTDOWN_TIMEOUT_SECONDS + 2, elapsed
        assert cache_path.read_text(encoding="utf-8") == sentinel
        for pid_path in (adapter_pid, probe_pid):
            try:
                _kill0(int(pid_path.read_text(encoding="utf-8")))
                assert False, f"{mode} process must be reaped"
            except ProcessLookupError:
                pass


def test_nonzero_exit_main_no_cache_write(executable: str, tmp_path: pathlib.Path) -> None:
    adapter_path = _write_adapter(
        tmp_path, "nonzero-main-adapter.py", _NONZERO_EXIT_AFTER_STOP_ADAPTER,
    )
    rom_path = tmp_path / "unused-rom-nonzero-main.bin"
    rom_path.write_bytes(b"\x00")
    cache_path = tmp_path / "nonzero-cache.json"
    sentinel = '{"sentinel": "nonzero-case"}\n'
    cache_path.write_text(sentinel, encoding="utf-8")

    old_rom_gate, old_musashi_gate = tool.check_rom_gate, tool.check_musashi_gate
    try:
        tool.check_rom_gate = lambda: (rom_path, None)
        tool.check_musashi_gate = lambda: (adapter_path, None)
        exit_code = tool.main(["--executable", executable, "--scan"], cache_path=cache_path)
    finally:
        tool.check_rom_gate, tool.check_musashi_gate = old_rom_gate, old_musashi_gate

    assert exit_code == tool.EXIT_ADAPTER_FAILURE, exit_code
    assert cache_path.read_text(encoding="utf-8") == sentinel, "must never overwrite an existing cache"


def main() -> None:
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        test_documented_adapter_ram_boundaries(tmp_path)
        with generous_protocol_timeout():
            test_clean_success_run_scan(executable, tmp_path)
        test_control_flow_depth_streaming_stop(executable, tmp_path)
        test_stop_decision_exact_thresholds_and_precedence()
        test_help_lists_actual_stop_precedence()
        with generous_protocol_timeout():
            test_clean_success_main_full_path(executable, tmp_path)
        test_early_unexpected_eof(executable, tmp_path)
        test_early_unexpected_eof_main_no_cache_write(executable, tmp_path)
        test_nonzero_exit_after_clean_stop(executable, tmp_path)
        test_nonzero_exit_main_no_cache_write(executable, tmp_path)
        test_malformed_protocol_not_json(executable, tmp_path)
        test_malformed_protocol_missing_key(executable, tmp_path)
        test_malformed_access_shapes_fail_before_probes(executable, tmp_path)
        test_malformed_entry_values_fail_before_probes_and_preserve_cache(executable, tmp_path)
        test_malformed_protocol_main_no_cache_write(executable, tmp_path)
        test_hanging_malformed_adapter_is_bounded_and_preserves_cache(executable, tmp_path)
        test_forking_sigterm_ignoring_malformed_adapter_is_group_reaped_and_preserves_cache(
            executable, tmp_path,
        )
        test_access_list_overflow_fails_before_probes_and_preserves_cache(executable, tmp_path)
        test_silent_adapter_protocol_wait_is_bounded_and_preserves_cache(executable, tmp_path)
        test_oversize_partial_protocol_line_fails_before_probes_and_preserves_cache(
            executable, tmp_path,
        )
        test_blank_flood_is_malformed_bounded_reaped_and_preserves_cache(executable, tmp_path)
        test_probe_failures_are_bounded_reaped_and_preserve_cache(executable, tmp_path)

    print("sonic startup inventory adapter failure test: ok")


if __name__ == "__main__":
    main()
