#!/usr/bin/env python3
"""SEG-108 (ADR 0252): Sega CD internal backup RAM persistence, end to end on project-authored fixtures.

Real generated-native programs (tools/sega_cd_fixture_disc.py fixture `backup_ram`, phases write / update / read) execute guest reads and writes of
internal backup RAM through the real sub-CPU bus: byte and word writes, a mirrored address, cells at both ends of the part. The launcher's own save
code (apps/segarecomp-launcher/sega_cd.cpp, driven through scd_save_tool) prepares the input, the program exports its candidate (`--backup-ram-out`), and
the launcher validates and commits it. Checked, each against an independent model of the cell layout written here:

  * a new game starts formatted, a save written by one session is read back exactly by the next (the guest sees the previous bytes), over several
    cycles, and across a "rebuild" (a different generated program for the same game folder);
  * a failed or killed run, a truncated candidate and an unwritable candidate leave the previous save byte for byte;
  * a corrupt or truncated or empty save is refused and kept; an unwritable game folder is reported; a legacy pristine image is a valid save;
  * replays and recordings read a defined scratch image and never touch the save; two games have independent saves.

usage: launcher_sega_cd_save_test.py <scd_save_tool> <segarecomp> <c-compiler> <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

TOOL, SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], sys.argv[3], pathlib.Path(sys.argv[4]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import sega_cd_common as common  # noqa: E402
import sega_cd_fixture_disc as fixture  # noqa: E402

failures = []
SIZE = 0x2000


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def cell_of(address):
    return ((address & ~1) - 0xFE0000) >> 1 & (SIZE - 1)  # odd byte lane, 8192 cells, mirrored every $4000 bytes of address


def apply_writes(image, writes):
    out = bytearray(image)
    for _size, address, value in writes:
        out[cell_of(address)] = value & 0xFF  # a byte write to an odd address, or a word write (stores its low byte)
    return bytes(out)


def expected_reads(image):
    values = []
    for size, address in fixture.BACKUP_READS:
        cell = image[cell_of(address)]
        values.append(cell if size == "b" else 0xFF00 | cell)
    return values


def formatted():
    trailer = b"_" * 11 + bytes([0, 0, 0, 0, 0x40]) + (125).to_bytes(2, "big") * 4 + bytes(8) + b"SEGA_CD_ROM" + bytes([0, 1, 0, 0, 0]) + b"RAM_CARTRIDGE___"
    assert len(trailer) == 64
    return bytes(SIZE - 64) + trailer


def tool(*args):
    out = subprocess.run([TOOL, *map(str, args)], capture_output=True, text=True, timeout=60)
    facts = dict(line.split("=", 1) for line in out.stdout.splitlines() if "=" in line)
    return out.returncode, facts


def guest(program, save, out=None, budget=6000000):
    """One session of a generated program as the launcher starts it. Returns (exit code, the guest's read-back registers or None)."""
    exe, disc = program
    extra = ["--backup-ram", str(save)] + (["--backup-ram-out", str(out)] if out is not None else [])
    code, report = common.run(exe, disc, budget, *extra)
    regs = None
    if report.get("cpus"):
        d = report["cpus"][1]["d"]
        regs = [d[i] & (0xFFFF if size == "w" else 0xFF) for i, (size, _a) in enumerate(fixture.BACKUP_READS)]
    return code, regs


def session(program, game, persist=True, budget=6000000):
    """prepare -> run -> finish, the launcher's lifecycle. Returns (prepare facts, exit code, registers, finish outcome)."""
    _, facts = tool("prepare", game, "persist" if persist else "scratch")
    if facts["status"] not in ("ok", "created"):
        return facts, None, None, None
    out = facts["backup_ram_out"] if facts["backup_ram_out"] != "-" else None
    code, regs = guest(program, facts["backup_ram"], out, budget)
    outcome = tool("finish", game, code)[1]["outcome"] if out is not None else "not_requested"
    return facts, code, regs, outcome


def main():
    with tempfile.TemporaryDirectory(prefix="scd-save-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        programs = {}
        for phase in ("write", "update", "read"):
            disc, exe, _, report = builder.program("bram_" + phase, "backup_ram", "cooked", "--backup-phase", phase)
            check(all(p["unsupported"] == 0 for p in report["processors"]), "%s program: every reached instruction compiled" % phase)
            programs[phase] = (exe, disc)
        base = formatted()
        state_a = apply_writes(base, fixture.BACKUP_WRITES["write"])
        state_b = apply_writes(state_a, fixture.BACKUP_WRITES["update"])
        check(len({base, state_a, state_b}) == 3 and state_a[0] == 0x5A and state_a[cell_of(0xFE0011)] == 0xC3 and state_a[1] == 0x77,
              "model: the two write sets change distinct cells (byte, word, mirror)")

        game = tmp / "lib" / "gameA"
        save = game / "backup-ram.bin"

        # 1. a new game: formatted, the guest reads the formatted state, writes pattern A; the candidate is committed
        check(tool("prepare", game, "persist")[1]["status"] == "created" and save.read_bytes() == base, "new game: the save is created formatted")
        facts, code, regs, outcome = session(programs["write"], game)
        check(code == 0 and regs == expected_reads(base), "new game: the guest reads the formatted cells (%s)" % regs)
        check(outcome == "stored" and save.read_bytes() == state_a, "session 1: the guest's writes are committed exactly")
        check(not (game / "backup-ram.next").exists(), "the candidate is consumed")
        check((game / "backup-ram.bin.prev").read_bytes() == base, "one previous generation is kept")

        # 2. reopen: the guest reads what the previous session wrote (and the rebuilt program is another program for the same game)
        facts, code, regs, outcome = session(programs["read"], game)
        check(facts["status"] == "ok" and code == 0 and regs == expected_reads(state_a), "session 2 (rebuilt program): the guest reads the previous bytes exactly (%s)" % regs)
        check(save.read_bytes() == state_a, "a session that writes nothing leaves the same bytes")

        # 3. a further cycle on top
        facts, code, regs, outcome = session(programs["update"], game)
        check(code == 0 and regs == expected_reads(state_a) and outcome == "stored" and save.read_bytes() == state_b, "session 3: updated on top of the persisted save")
        facts, code, regs, outcome = session(programs["read"], game)
        check(regs == expected_reads(state_b), "session 4: the second cycle's bytes survive (%s)" % regs)

        # 4. failed child: budget exhausted (11) with a candidate on disk, killed (137): the previous save survives byte for byte
        facts, code, regs, outcome = session(programs["write"], game, budget=20)
        check(code == 11 and outcome == "kept_run_failed" and save.read_bytes() == state_b and not (game / "backup-ram.next").exists(),
              "budget-exhausted run: exit %s, candidate discarded, previous save kept" % code)
        _, facts = tool("prepare", game, "persist")
        (game / "backup-ram.next").write_bytes(state_a)
        check(tool("finish", game, 137)[1]["outcome"] == "kept_run_failed" and save.read_bytes() == state_b and not (game / "backup-ram.next").exists(), "killed child: previous save kept")
        # truncated candidate (interrupted export), exit status 0
        (game / "backup-ram.next").write_bytes(state_a[:5000])
        check(tool("finish", game, 0)[1]["outcome"] == "kept_commit_failed" and save.read_bytes() == state_b and not (game / "backup-ram.next").exists(),
              "truncated candidate: refused, previous save kept")
        # an interrupted earlier session left a candidate: the next start discards it
        (game / "backup-ram.next").write_bytes(state_a)
        tool("prepare", game, "persist")
        check(not (game / "backup-ram.next").exists() and save.read_bytes() == state_b, "a stale candidate is never taken for the next run's")
        # unwritable candidate destination: the program reports it and exits 28; the save is untouched
        (game / "blocked").mkdir()
        code, _ = guest(programs["write"], save, game / "blocked")
        check(code == 28 and save.read_bytes() == state_b, "unwritable candidate: exit 28, previous save kept")

        # 5. replay / recording: a defined scratch image, never the save; nothing exported; deterministic
        facts, code, regs, outcome = session(programs["read"], game, persist=False)
        check(facts["status"] == "created" and facts["backup_ram_out"] == "-" and pathlib.Path(facts["backup_ram"]) != save, "replay: scratch input, no export")
        check(code == 0 and regs == expected_reads(base) and outcome == "not_requested", "replay: the guest sees the defined formatted image (%s)" % regs)
        facts, code, regs, _ = session(programs["write"], game, persist=False)
        check(save.read_bytes() == state_b and regs == expected_reads(base), "a replay that writes leaves the canonical save unchanged and the next replay identical")
        check(pathlib.Path(facts["backup_ram"]).read_bytes() == base, "the scratch image is rewritten formatted each time")

        # 6. corrupt, truncated, empty, oversized saves are refused and kept
        for label, content in (("truncated", state_b[:100]), ("empty", b""), ("oversized", state_b + b"\0")):
            save.write_bytes(content)
            facts, code, regs, outcome = session(programs["read"], game)
            check(facts["status"] == "wrong_size" and code is None and save.read_bytes() == content, "%s save: reported, not started, kept as it was" % label)
        save.write_bytes(state_b)
        save.unlink()
        save.mkdir()
        facts = tool("prepare", game, "persist")[1]
        check(facts["status"] == "unreadable" and save.is_dir(), "a save that is not a file is unreadable and untouched")
        save.rmdir()
        save.write_bytes(state_b)

        # 7. an unwritable game folder is reported, not ignored
        (tmp / "afile").write_bytes(b"x")
        check(tool("prepare", tmp / "afile" / "sub", "persist")[1]["status"] == "not_writable", "unwritable destination is reported")

        # 8. legacy pristine image (an earlier launcher's file) is a valid save; any 8 KiB content is accepted as it is
        legacy = tmp / "lib" / "legacy"
        legacy.mkdir(parents=True)
        (legacy / "backup-ram.bin").write_bytes(base)
        facts, code, regs, outcome = session(programs["write"], legacy)
        check(facts["status"] == "ok" and regs == expected_reads(base) and outcome == "stored" and (legacy / "backup-ram.bin").read_bytes() == state_a, "legacy pristine image: loaded, then updated")
        zero = tmp / "lib" / "zeros"
        zero.mkdir(parents=True)
        (zero / "backup-ram.bin").write_bytes(bytes(SIZE))
        facts, code, regs, outcome = session(programs["read"], zero)
        check(facts["status"] == "ok" and regs == expected_reads(bytes(SIZE)), "an all-zero 8 KiB save is loaded unchanged (never reformatted)")

        # 9. games are independent
        other = tmp / "lib" / "gameB"
        facts, code, regs, outcome = session(programs["write"], other)
        check(facts["status"] == "created" and regs == expected_reads(base) and (other / "backup-ram.bin").read_bytes() == state_a and save.read_bytes() == state_b,
              "a second game starts formatted and saves independently")

        # 10. runtime: the export is opt-in
        code, report = common.run(*programs["read"][:1], programs["read"][1], 6000000, "--backup-ram", str(save))
        check(code == 0 and not (game / "backup-ram.next").exists(), "without --backup-ram-out nothing is exported")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
