#!/usr/bin/env python3
"""SEG-021-T041 / ADR 0049: lifecycle and accounting of the optional copy-alias PREPARATION phase.

Project-authored, hermetic. The bridge's build/run steps are replaced by an in-process synthetic "guest" so the
orchestration itself is what is exercised: `--discover-copy-aliases` is an optional bounded preparation phase (each
round = one generation + one compile + one run) that precedes the canonical final one-shot phase (exactly one
generation, one compile, one run). Every path of the termination vocabulary is checked, including fail-closed
max-round exhaustion and tool failure, and that the plain --one-shot invocation has no preparation at all.
"""
import contextlib
import io
import json
import os
import pathlib
import random
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import genesis_startup_bridge as b  # noqa: E402

random.seed(11)
ROM = bytes(random.randrange(256) for _ in range(0x8000))
RAM_BEGIN = b.GENESIS_WORK_RAM_BEGIN
# Each stage is a ROM->RAM executable copy the guest reaches in order: (ram_offset, rom_offset, length).
THUNK_OFFSET = 0x0A00
THUNK_BYTES = bytes.fromhex("4ef900000200")    # observed JMP stub (recognition is the CPU decoder's; here a stand-in)
THUNK_CHANGED = bytes.fromhex("4ef900000400")  # the same address, later observation: a different target
STAGES = [(0x0400, 0x1230, 0x40), (0x0800, 0x2460, 0x40), (0x0C00, 0x3000, 0x40)]


class Guest:
    """Synthetic guest. The stop reached by a program depends only on the aliases in its emitter command."""

    def __init__(self, stages, end="runner", ignore_aliases=False, corrupt=False, stop_class=5, pc_outside=False, thunk_mode=None):
        self.thunk_mode = thunk_mode  # None | "advance" | "same" | "changed": scripted ADR 0097 RAM-thunk scenarios
        self.classify_calls = []
        self.thunk_programs = {}
        self.stages, self.end, self.ignore_aliases, self.corrupt = stages, end, ignore_aliases, corrupt
        self.stop_class, self.pc_outside = stop_class, pc_outside
        self.generations = []      # emitter commands, in order
        self.runs = []             # (executable, is_preparation_run)
        self.programs = {}         # executable path -> number of aliases
        self.fail_generation_at = None

    def generate(self, command, compiler, root, out_dir, debug, viewer_sdl3=None, capture=False):
        self.generations.append(list(command))
        if self.fail_generation_at == len(self.generations):
            return 1, None, None
        covered = sum(1 for arg in command if arg == "--immutable-copy-alias")
        executable = out_dir / f"bridge{len(self.generations)}"
        self.programs[str(executable)] = covered
        self.thunk_programs[str(executable)] = sum(1 for arg in command if arg == "--ram-thunk")
        return 0, b"", executable

    def _stop_index(self, executable):
        return 0 if self.ignore_aliases else self.programs[str(executable)]

    def run_capture(self, command, **kwargs):
        env = kwargs["env"]
        self.runs.append((command[0], "SEGARECOMP_STOP_WORK_RAM_DUMP" in env))
        index = self._stop_index(command[0])

        class Completed:
            returncode = 0
            stdout = ""
        result = Completed()
        if self.thunk_mode:
            # The guest needs one absolute-JMP stub at RAM_BEGIN + THUNK_OFFSET. Before it is materialized the run stops there; after,
            # "advance" completes the window, "same" stops there again with the same bytes, "changed" with different bytes.
            built = self.thunk_programs[str(command[0])]
            if built and self.thunk_mode == "advance":
                result.stderr = "CAPTURE_SUMMARY " + json.dumps({"outcome": "window_complete"}) + "\n"
                return result
            ram = bytearray(random.Random(11).randbytes(b.GENESIS_WORK_RAM_SIZE))
            ram[THUNK_OFFSET:THUNK_OFFSET + 6] = THUNK_CHANGED if built and self.thunk_mode == "changed" else THUNK_BYTES
            pathlib.Path(env["SEGARECOMP_STOP_WORK_RAM_DUMP"]).write_bytes(
                (RAM_BEGIN + THUNK_OFFSET).to_bytes(4, "big") + (5).to_bytes(4, "big") + bytes(ram))
            result.stderr = "CAPTURE_SUMMARY " + json.dumps({"outcome": "incomplete_guest_stop"}) + "\n"
            return result
        if index >= len(self.stages):
            outcome = {"runner": "incomplete_runner_resource_limit", "window": "window_complete"}[self.end]
            result.stderr = "CAPTURE_SUMMARY " + json.dumps({"outcome": outcome}) + "\n"
            return result
        ram = bytearray(random.Random(5).randbytes(b.GENESIS_WORK_RAM_SIZE))
        if not self.corrupt:
            for offset, source, size in self.stages[:index + 1]:
                ram[offset:offset + size] = ROM[source:source + size]
        pc = 0x00001000 if self.pc_outside else RAM_BEGIN + self.stages[index][0]
        pathlib.Path(env["SEGARECOMP_STOP_WORK_RAM_DUMP"]).write_bytes(
            pc.to_bytes(4, "big") + self.stop_class.to_bytes(4, "big") + bytes(ram))
        result.stderr = "CAPTURE_SUMMARY " + json.dumps({"outcome": "incomplete_guest_stop"}) + "\n"
        return result

    def classify(self, emitter_command, pc, window_hex):
        """Stand-in for the CPU-owned recognizer seam (never spawns a process): exactly THUNK_BYTES is a JMP stub."""
        self.classify_calls.append((pc, window_hex))
        return THUNK_BYTES.hex() if self.thunk_mode and window_hex.startswith(THUNK_BYTES.hex()) else None

    def run_bridge(self, executable, root, **kwargs):
        self.runs.append((str(executable), False))
        covered = self._stop_index(str(executable)) >= len(self.stages)
        report = {"result": "completed" if covered else "stop", "stop_class": "known_but_unemitted_target"}
        return 0, report, b"", "", b""


def invoke(guest, *flags, rom_path, out_dir, env=None):
    argv = ["genesis_startup_bridge.py", "--segarecomp", str(pathlib.Path(__file__)), "--cc", sys.executable,
            "--rom", str(rom_path), "--mode", "commercial", "--one-shot", "--diagnose-frontier",
            "--immutable-rom-aot", "--out-dir", str(out_dir), *flags]
    saved = {name: getattr(b, name) for name in (
        "classify_ram_jump_thunk", "generate_and_compile", "run_bridge", "report_sha_status", "valid_sanitized", "parse_canonical_full",
        "valid_full", "offline_inventory_stitch_metrics", "ephemeral_frontier", "with_execution_history",
        "parse_ephemeral_pc_history", "write_combined_diagnosis")}
    original_run = b.subprocess.run
    b.classify_ram_jump_thunk = guest.classify
    b.generate_and_compile = guest.generate
    b.run_bridge = guest.run_bridge
    b.report_sha_status = lambda report, digest: 0
    b.valid_sanitized = lambda report, digest: True
    b.parse_canonical_full = lambda data: {"result": "x"}
    b.valid_full = lambda full, report, digest: True
    b.offline_inventory_stitch_metrics = lambda directory: {}
    b.ephemeral_frontier = lambda *args: {}
    b.with_execution_history = lambda ephemeral, data: ephemeral
    b.parse_ephemeral_pc_history = lambda data: []
    b.write_combined_diagnosis = lambda *args: None
    b.subprocess.run = lambda command, **kwargs: (
        guest.run_capture(command, **kwargs) if str(command[0]) in guest.programs else original_run(command, **kwargs))
    old_argv, old_env = sys.argv, dict(os.environ)
    os.environ.update(env or {})
    stderr, stdout = io.StringIO(), io.StringIO()
    try:
        sys.argv = argv
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(stdout):
            status = b.main()
    finally:
        sys.argv = old_argv
        os.environ.clear()
        os.environ.update(old_env)
        b.subprocess.run = original_run
        for name, value in saved.items():
            setattr(b, name, value)
    return status, stderr.getvalue()


def line(stderr, prefix):
    found = [json.loads(text[len(prefix) + 1:]) for text in stderr.splitlines() if text.startswith(prefix + " ")]
    assert len(found) <= 1, (prefix, found)
    return found[0] if found else None


def thunk_args(command):
    return [command[i + 1] for i, arg in enumerate(command) if arg == "--ram-thunk"]


def alias_args(command):
    return [command[i + 1] for i, arg in enumerate(command) if arg == "--immutable-copy-alias"]


def main():
    assert (b.ALIAS_TERMINATION_ROUTE_ADVANCED, b.ALIAS_TERMINATION_NO_WORK_RAM_FRONTIER,
            b.ALIAS_TERMINATION_NOT_VERBATIM_COPY, b.ALIAS_TERMINATION_REPEATED_ALIAS,
            b.ALIAS_TERMINATION_RUNNER_RESOURCE_LIMIT, b.ALIAS_TERMINATION_NON_ALIAS_FRONTIER,
            b.ALIAS_TERMINATION_MAX_ROUNDS, b.ALIAS_TERMINATION_TOOL_FAILURE) == (
        "route_advanced", "no_work_ram_frontier", "frontier_not_verbatim_copy", "repeated_alias_no_progress",
        "runner_resource_limit", "non_alias_frontier", "max_rounds", "tool_failure")
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=build, prefix="genesis-copy-alias-lifecycle-") as directory:
        work = pathlib.Path(directory)
        rom = work / "synthetic.bin"
        rom.write_bytes(ROM)
        counter = [0]

        def run(guest, *flags, env=None):
            counter[0] += 1
            return invoke(guest, *flags, rom_path=rom, out_dir=work / f"out{counter[0]}", env=env)

        # 1. Plain --one-shot: no preparation, no work-RAM dump, no summary, exactly one final cycle.
        guest = Guest([])
        status, err = run(guest)
        assert status == 0, err
        assert line(err, "COPY_ALIAS_DISCOVERY") is None
        assert len(guest.generations) == 1 and guest.runs == [(str(work / "out1" / "bridge1"), False)], guest.runs
        assert not any("--immutable-copy-alias" in command for command in guest.generations)
        assert line(err, "ONE_SHOT_SUMMARY")["generation_attempts"] == 1
        # A guest that needs an alias still stops fail-closed without the flag: no hidden discovery happens.
        guest = Guest(STAGES[:1])
        status, err = run(guest)
        assert len(guest.generations) == 1 and len(guest.runs) == 1
        assert line(err, "COPY_ALIAS_DISCOVERY") is None
        assert line(err, "ONE_SHOT_SUMMARY")["driver_result"] == "offline_inventory_incomplete"

        # 2. Discovery with zero aliases: one summary with real counts, then one final cycle.
        guest = Guest([])
        status, err = run(guest, "--discover-copy-aliases")
        assert status == 0, err
        assert line(err, "COPY_ALIAS_DISCOVERY") == {
            "rounds": 1, "generation_attempts": 1, "compile_attempts": 1, "run_attempts": 1, "alias_count": 0,
            "alias_total_bytes": 0, "termination_reason": "runner_resource_limit"}
        assert len(guest.generations) == 2 and not alias_args(guest.generations[1])
        assert line(err, "ONE_SHOT_SUMMARY")["generation_attempts"] == 1
        assert [prep for _, prep in guest.runs] == [True, False]

        # 3. One alias: found in round 1, confirmed in round 2, final program built once with exactly that alias.
        guest = Guest(STAGES[:1], end="window")
        status, err = run(guest, "--discover-copy-aliases")
        assert status == 0, err
        summary = line(err, "COPY_ALIAS_DISCOVERY")
        assert summary == {"rounds": 2, "generation_attempts": 2, "compile_attempts": 2, "run_attempts": 2,
                           "alias_count": 1, "alias_total_bytes": 0x40, "termination_reason": "route_advanced"}, summary
        assert len(guest.generations) == 3
        assert alias_args(guest.generations[0]) == [] and len(alias_args(guest.generations[1])) == 1
        assert alias_args(guest.generations[2]) == alias_args(guest.generations[1])
        assert [prep for _, prep in guest.runs] == [True, True, False]
        one_shot = line(err, "ONE_SHOT_SUMMARY")
        assert one_shot["generation_attempts"] == 1 and one_shot["compile_attempts"] == 1
        assert one_shot["driver_result"] == "completed"  # the final program (with the alias) completes
        assert not re.search(r"[0-9a-f]{8}", json.dumps(summary))

        # 4. Two sequential aliases A then B.
        guest = Guest(STAGES[:2])
        status, err = run(guest, "--discover-copy-aliases")
        summary = line(err, "COPY_ALIAS_DISCOVERY")
        assert status == 0 and summary["rounds"] == 3 and summary["alias_count"] == 2, (summary, err)
        assert summary["generation_attempts"] == summary["compile_attempts"] == summary["run_attempts"] == 3
        assert summary["alias_total_bytes"] == 0x80 and len(guest.generations) == 4
        assert len(alias_args(guest.generations[3])) == 2

        # 5. An alias that makes no progress terminates the preparation (proposal already known).
        guest = Guest(STAGES[:1], ignore_aliases=True)
        status, err = run(guest, "--discover-copy-aliases")
        summary = line(err, "COPY_ALIAS_DISCOVERY")
        assert summary["termination_reason"] == "repeated_alias_no_progress" and summary["rounds"] == 2
        assert summary["alias_count"] == 1 and status == 0

        # 6. Frontiers that no alias explains: non-verbatim RAM code, a non-work-RAM stop, another stop class.
        for guest, reason in ((Guest(STAGES[:1], corrupt=True), "frontier_not_verbatim_copy"),
                              (Guest(STAGES[:1], pc_outside=True), "no_work_ram_frontier"),
                              (Guest(STAGES[:1], stop_class=3), "non_alias_frontier")):
            status, err = run(guest, "--discover-copy-aliases")
            summary = line(err, "COPY_ALIAS_DISCOVERY")
            assert summary["termination_reason"] == reason and summary["alias_count"] == 0 and status == 0, (reason, summary)
            assert len(guest.generations) == 2  # one preparation round + the final one-shot build

        # 7. Bound exhaustion fails closed: nonzero exit, incomplete preparation, no final program built or run.
        guest = Guest(STAGES)
        status, err = run(guest, "--discover-copy-aliases", env={b.ALIAS_MAX_ROUNDS_ENV: "2"})
        summary = line(err, "COPY_ALIAS_DISCOVERY")
        assert status == b.GENESIS_EXIT_ALIAS_PREPARATION_INCOMPLETE, (status, err)
        assert summary["termination_reason"] == "max_rounds" and summary["rounds"] == 2 and summary["alias_count"] == 2
        assert len(guest.generations) == 2 and all(prep for _, prep in guest.runs)
        assert line(err, "ONE_SHOT_SUMMARY") is None and "incomplete" in err
        # The same guest completes with a sufficient bound (3 stages need 4 rounds).
        guest = Guest(STAGES)
        status, err = run(guest, "--discover-copy-aliases", env={b.ALIAS_MAX_ROUNDS_ENV: "4"})
        assert status == 0 and line(err, "COPY_ALIAS_DISCOVERY")["rounds"] == 4

        # 8. A build failure during preparation fails closed with real attempt counts.
        guest = Guest(STAGES[:1])
        guest.fail_generation_at = 1
        status, err = run(guest, "--discover-copy-aliases")
        assert status == 2
        assert line(err, "COPY_ALIAS_DISCOVERY") == {
            "rounds": 1, "generation_attempts": 1, "compile_attempts": 0, "run_attempts": 0, "alias_count": 0,
            "alias_total_bytes": 0, "termination_reason": "tool_failure"}
        assert len(guest.generations) == 1 and line(err, "ONE_SHOT_SUMMARY") is None

        # 9. ADR 0097 RAM thunks (reproducible materialization). First observation materializes exactly the recognized bytes and the
        # next round carries it; the route then advances.
        guest = Guest([], thunk_mode="advance")
        status, err = run(guest, "--discover-copy-aliases")
        summary = line(err, "COPY_ALIAS_DISCOVERY")
        assert status == 0 and summary["termination_reason"] == "route_advanced" and summary["ram_thunk_count"] == 1, (summary, err)
        assert summary["ram_thunk_total_bytes"] == 6 and summary["alias_count"] == 0
        assert thunk_args(guest.generations[1]) == [f"{RAM_BEGIN + THUNK_OFFSET:08x}:{THUNK_BYTES.hex()}"]
        assert thunk_args(guest.generations[-1]) == thunk_args(guest.generations[1])  # the final program is built with it
        assert len(guest.classify_calls) == 1
        # Identical bytes observed again at the materialized address: deterministic, no new thunk, no progress (the preparation ends,
        # the final program is built with the one thunk, which stays fail-closed at runtime).
        guest = Guest([], thunk_mode="same")
        status, err = run(guest, "--discover-copy-aliases")
        summary = line(err, "COPY_ALIAS_DISCOVERY")
        assert status == 0 and summary["termination_reason"] == "repeated_alias_no_progress" and summary["ram_thunk_count"] == 1, summary
        assert len(guest.classify_calls) == 1 and len(thunk_args(guest.generations[-1])) == 1
        # Changed bytes at the same materialized address: typed, INCOMPLETE; no final program is generated, built or run, and the
        # changed target is never offered to the emitter (every generated command carries only the first observation's bytes).
        guest = Guest([], thunk_mode="changed")
        status, err = run(guest, "--discover-copy-aliases")
        summary = line(err, "COPY_ALIAS_DISCOVERY")
        assert status == 2 and summary["termination_reason"] == "ram_thunk_mismatch" and summary["ram_thunk_count"] == 1, (status, summary)
        assert "ram_thunk_mismatch" in b.ALIAS_INCOMPLETE_TERMINATIONS
        assert line(err, "ONE_SHOT_SUMMARY") is None and "incomplete" in err
        assert len(guest.generations) == 2 and all(prep for _, prep in guest.runs), guest.runs  # only preparation rounds ran
        assert all(THUNK_CHANGED.hex() not in " ".join(command) for command in guest.generations)
        assert len(guest.classify_calls) == 1  # the changed bytes were not even offered to the recognizer
        # Existing copy-alias behaviour is unchanged by all of the above (cases 1-8) and no alias run ever consults the recognizer.
        guest = Guest(STAGES[:1], end="window")
        run(guest, "--discover-copy-aliases")
        assert guest.classify_calls == []
    print("genesis_copy_alias_lifecycle_test: OK")


if __name__ == "__main__":
    main()
