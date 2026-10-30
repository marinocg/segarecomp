# ADR 0252: SEG-108 M12 - persistent Sega CD internal backup RAM

- Status: T001 contract accepted; implementation T002-T004; decision recorded in "Decision" (SEG-108 / M12, one combined PR, internal tasks T001-T005).
- Related: ADR 0147 (the 8 KiB internal backup RAM machine input and the formatted image), ADR 0204 (launcher integration: the game's run folder, `--backup-ram`), ADR 0251 (M11; product PR #172 verified merged before this work began).
- Scope: backup RAM persistence only. Not emulator save states, rewind, cloud sync, multiple slots/profiles, external RAM cartridges, game-specific editors, or any CPU/VDP/CD/PCM/mixer change.
- Privacy: no save data, ROM bytes, BIOS images or traces are committed; the tests author their own bytes.

## T001 - the save persistence contract (gate `BACKUP_PERSISTENCE_CONTRACT_GO`)

What was found. The hardware model already implements the 8 KiB part (odd byte lane, mirrored, word access, guest reads and writes) and takes an image with `--backup-ram`. The launcher passed `<game folder>/backup-ram.bin` but `ensure_backup_ram()` **replaced any file that differed from the pristine formatted image** - persistence by writing modified RAM back would have been erased at the next start. The program also never exported its final RAM. The per-game folder is `<library>/sega-cd/<first 16 hex of the library entry's identity>/`; the generated program lives in its `build/` subfolder, so a rebuild or re-import does not touch `backup-ram.bin`.

Contract (each point is implemented and tested):

1. A new game starts from the existing formatted 8 KiB image (the formatter is unchanged).
2. An existing regular file of exactly 8 KiB is loaded **unchanged whatever it contains** (all zero and unformatted included); the file is never reformatted or compared with the pristine image.
3. A missing save is created with the formatter, through a temporary file and a rename.
4. A save of any other length, an unreadable one, or one that is not a regular file is an **error**: the game is not started, the user is told, and the file is left exactly as it was.
5. Normal interactive play (Play with no recording) reads the canonical save and asks the program to export its final RAM to a candidate (`backup-ram.next`) in the same folder.
6. Replays, "No window" replays, recordings and the build pass never read or write the canonical save. A replay or recording starts from a scratch image (`replay-backup-ram.bin`, rewritten formatted each run) and requests no export, so repeated runs are deterministic and a recording replays against the same defined image. Consequence: a recorded session does not load or store the player's progress.
7. Only a run that ends with status 0 is committed. The candidate is validated (regular file, exactly 8 KiB) and installed with one rename onto the save; any other end (typed stop 10, budget 11, refusal 20-28, crash, kill), an invalid or missing candidate, or a failed rename keeps the previous save and discards the candidate. The previous generation is also kept as `backup-ram.bin.prev` (best effort). A stale candidate left by a crash is deleted before every start and is never committed. The launcher reports the real outcome: "Save data stored." only after the rename succeeded; otherwise a visible message that the previous save was kept.
8. The save belongs to the game-library identity (the folder keyed by the entry), not to the generated program or its build identity; rebuilding, re-importing or replacing the program keeps it.

Atomicity. `std::filesystem::rename` replaces an existing file atomically on POSIX and, on Windows, through `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`. The target is never deleted first. The temporary file and the target are in the same folder (same volume). The data is flushed to the OS before the rename; fsync-level power-loss durability is **not** claimed (the previous generation file limits the exposure). This is the same pattern the launcher already uses for its library and settings files.

Rejected: a save directory/database, serialization of the machine, a background save service or thread (the existing process lifecycle already gives one natural commit point - the child's exit), and committing on non-zero exits (an uncertain machine state is not progress worth overwriting a good save with).

Gate: **`BACKUP_PERSISTENCE_CONTRACT_GO`**.

## T002 - runtime export

`--backup-ram-out FILE` (independent of `--backup-ram`): after the run ends, before the report is written, the program writes exactly `SCD_BACKUP_RAM_BYTES` (8192) bytes of `ScdMachine::backup_ram` - no header, compression or metadata - to FILE. It is accepted only together with `--backup-ram` (valid backup RAM installed; otherwise usage status 2 before any instruction). A failure to write it is the typed exit status 28; the input image is never touched and the program never writes the launcher's canonical save (the launcher chooses the candidate path). Nothing is exported unless requested, the report is byte-identical with or without the option, and guest memory, CPU execution and scheduling are untouched. The candidate is exported for every result (idle, stop, budget) so that the launcher, not the runtime, decides what to keep.

## T003 - launcher

`prepare_backup_ram` / `finish_backup_ram` / `commit_backup_ram` (`apps/segarecomp-launcher/sega_cd.cpp`) implement points 1-8; the GUI (`scd_play`, `scd_finish_run`), the headless `--sega-cd` mode and the build pass share them. Exit status 28 is a typed program status.

## T004 - validation

- `tests/sega_cd_backup_ram_test.py` (hermetic): the export from a generated-native program whose sub CPU writes through the real bus; exact bytes, unchanged report, opt-in, usage and unwritable-destination statuses, input untouched.
- `apps/segarecomp-launcher/launcher_sega_cd_save_test.py` (launcher build): generated-native programs read and write backup RAM through the real bus (byte, word, mirrored address, both ends of the part); the launcher's own save code is driven through `scd_save_tool`. Several save/load cycles with exact read-back by the guest, a "rebuilt" program for the same game, a budget-exhausted and a killed child, a truncated candidate, an unwritable candidate and destination, corrupt/truncated/empty/oversized/non-file saves, a legacy pristine image, an all-zero save, replays that write and leave the save unchanged, two independent games.
- `launcher_sega_cd_test` (C++): the same cases at the function level, run arguments, the stale-candidate and failed-rename paths.
- Fixture `backup_ram` (`tools/sega_cd_fixture_disc.py`, phases write/update/read) is project-authored.

## Not supported

Save states, rewind, cloud sync, several slots/profiles, external RAM cartridges, game-specific editing, saving from a recorded session, closing the launcher window during play (the child is killed, so that session's progress is not stored and the previous save is kept), saving after a typed stop or a budget-limited run, fsync-level durability across power loss, and concurrent launcher instances playing the same game (the busy-run guard already forbids it inside one launcher).
